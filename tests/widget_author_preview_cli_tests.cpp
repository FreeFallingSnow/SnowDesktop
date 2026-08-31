#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

std::wstring Quote(std::wstring_view value)
{
    return L"\"" + std::wstring(value) + L"\"";
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t root[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH, root) != 0,
            "temporary root is available");
        path = std::filesystem::path(root) /
            (L"SnowDesktopPreviewTest-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        std::error_code error;
        Check(std::filesystem::create_directory(path, error),
            "temporary preview directory is created");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

std::pair<int, std::string> Run(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    Check(CreatePipe(&readPipe, &writePipe, &security, 0) != FALSE,
        "preview output pipe is created");
    Check(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) != FALSE,
        "preview output read handle is private to the test");

    std::wstring command = Quote(executable.wstring());
    for (const auto& argument : arguments)
        command += L" " + Quote(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL launched = CreateProcessW(executable.c_str(),
        mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    Check(launched != FALSE, "snowwidget preview process starts");
    CloseHandle(process.hThread);

    std::string output;
    std::array<char, 1024> buffer{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer.data(),
            static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        output.append(buffer.data(), read);
    CloseHandle(readPipe);
    Check(WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0,
        "snowwidget preview process completes");
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return { static_cast<int>(exitCode), std::move(output) };
}

void CheckPng(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(static_cast<bool>(input), "preview PNG exists");
    std::array<unsigned char, 8> signature{};
    input.read(reinterpret_cast<char*>(signature.data()), signature.size());
    constexpr std::array<unsigned char, 8> expected{
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    Check(signature == expected, "preview output has a PNG signature");
    std::error_code error;
    Check(std::filesystem::file_size(path, error) > 512 && !error,
        "real renderer writes nontrivial preview pixels");
}

struct PixelBounds
{
    int left = std::numeric_limits<int>::max();
    int top = std::numeric_limits<int>::max();
    int right = -1;
    int bottom = -1;
    std::size_t count = 0;

    void Include(int x, int y)
    {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x);
        bottom = std::max(bottom, y);
        ++count;
    }

    bool Empty() const noexcept { return count == 0; }
};

struct RgbaBitmap
{
    UINT width = 0;
    UINT height = 0;
    std::vector<std::uint8_t> pixels;
};

std::array<std::uint8_t, 4> PixelAt(
    const RgbaBitmap& bitmap, UINT x, UINT y)
{
    Check(x < bitmap.width && y < bitmap.height,
        "preview pixel coordinate is in bounds");
    const std::size_t offset =
        (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
    return {
        bitmap.pixels[offset], bitmap.pixels[offset + 1],
        bitmap.pixels[offset + 2], bitmap.pixels[offset + 3] };
}

std::size_t CountDifferingPixels(const RgbaBitmap& first,
    const RgbaBitmap& second, const RECT& bounds)
{
    Check(first.width == second.width && first.height == second.height &&
            bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= static_cast<LONG>(first.width) &&
            bounds.bottom <= static_cast<LONG>(first.height) &&
            bounds.left < bounds.right && bounds.top < bounds.bottom,
        "preview difference bounds are valid");
    std::size_t differences = 0;
    for (LONG y = bounds.top; y < bounds.bottom; ++y)
    {
        for (LONG x = bounds.left; x < bounds.right; ++x)
        {
            if (PixelAt(first, static_cast<UINT>(x), static_cast<UINT>(y)) !=
                PixelAt(second, static_cast<UINT>(x), static_cast<UINT>(y)))
            {
                ++differences;
            }
        }
    }
    return differences;
}

std::size_t CountDarkenedPixels(const RgbaBitmap& baseline,
    const RgbaBitmap& highlighted, const RECT& bounds)
{
    Check(baseline.width == highlighted.width &&
            baseline.height == highlighted.height &&
            bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= static_cast<LONG>(baseline.width) &&
            bounds.bottom <= static_cast<LONG>(baseline.height) &&
            bounds.left < bounds.right && bounds.top < bounds.bottom,
        "preview darkening bounds are valid");
    std::size_t darkened = 0;
    for (LONG y = bounds.top; y < bounds.bottom; ++y)
    {
        for (LONG x = bounds.left; x < bounds.right; ++x)
        {
            const auto before = PixelAt(
                baseline, static_cast<UINT>(x), static_cast<UINT>(y));
            const auto after = PixelAt(
                highlighted, static_cast<UINT>(x), static_cast<UINT>(y));
            if (after[0] < before[0] || after[1] < before[1] ||
                after[2] < before[2])
                ++darkened;
        }
    }
    return darkened;
}

std::uint64_t SumBrightnessGain(const RgbaBitmap& baseline,
    const RgbaBitmap& highlighted, const RECT& bounds)
{
    Check(baseline.width == highlighted.width &&
            baseline.height == highlighted.height &&
            bounds.left >= 0 && bounds.top >= 0 &&
            bounds.right <= static_cast<LONG>(baseline.width) &&
            bounds.bottom <= static_cast<LONG>(baseline.height) &&
            bounds.left < bounds.right && bounds.top < bounds.bottom,
        "preview brightness bounds are valid");
    std::uint64_t gain = 0;
    for (LONG y = bounds.top; y < bounds.bottom; ++y)
    {
        for (LONG x = bounds.left; x < bounds.right; ++x)
        {
            const auto before = PixelAt(
                baseline, static_cast<UINT>(x), static_cast<UINT>(y));
            const auto after = PixelAt(
                highlighted, static_cast<UINT>(x), static_cast<UINT>(y));
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                gain += after[channel] > before[channel]
                    ? static_cast<std::uint64_t>(
                        after[channel] - before[channel])
                    : 0u;
            }
        }
    }
    return gain;
}

void WriteSolidBmp(const std::filesystem::path& path,
    std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    constexpr LONG width = 8;
    constexpr LONG height = 8;
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER infoHeader{};
    infoHeader.biSize = sizeof(infoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = height;
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = width * height * 4;
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + infoHeader.biSizeImage;
    const std::uint32_t pixel = 0xff000000u |
        static_cast<std::uint32_t>(blue) |
        (static_cast<std::uint32_t>(green) << 8) |
        (static_cast<std::uint32_t>(red) << 16);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * height, pixel);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    Check(static_cast<bool>(output), "custom background BMP is created");
    output.write(reinterpret_cast<const char*>(&fileHeader),
        sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader),
        sizeof(infoHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size() * sizeof(pixel)));
    Check(static_cast<bool>(output), "custom background BMP is written");
}

RgbaBitmap ReadPng(const std::filesystem::path& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))),
        "WIC factory is created");
    ComPtr<IWICBitmapDecoder> decoder;
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
              GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)),
        "preview PNG decoder is created");
    ComPtr<IWICBitmapFrameDecode> frame;
    Check(SUCCEEDED(decoder->GetFrame(0, &frame)),
        "preview PNG frame is available");
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(),
                GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom)),
        "preview PNG converts to RGBA pixels");

    RgbaBitmap bitmap;
    Check(SUCCEEDED(converter->GetSize(&bitmap.width, &bitmap.height)) &&
            bitmap.width > 0 && bitmap.height > 0,
        "preview PNG has nonzero dimensions");
    const UINT stride = bitmap.width * 4;
    bitmap.pixels.resize(static_cast<std::size_t>(stride) * bitmap.height);
    Check(SUCCEEDED(converter->CopyPixels(nullptr, stride,
              static_cast<UINT>(bitmap.pixels.size()),
              bitmap.pixels.data())),
        "preview PNG pixels are decoded");
    return bitmap;
}

RgbaBitmap CheckOpaquePreview(const std::filesystem::path& path,
    UINT expectedWidth, UINT expectedHeight)
{
    const RgbaBitmap bitmap = ReadPng(path);
    Check(bitmap.width == expectedWidth && bitmap.height == expectedHeight,
        "preview stage preserves the expected pixel dimensions");
    std::unordered_set<std::uint32_t> colors;
    for (std::size_t offset = 0; offset < bitmap.pixels.size(); offset += 4)
    {
        Check(bitmap.pixels[offset + 3] == 0xff,
            "preview stage makes every exported pixel opaque");
        colors.insert(
            (static_cast<std::uint32_t>(bitmap.pixels[offset]) << 16) |
            (static_cast<std::uint32_t>(bitmap.pixels[offset + 1]) << 8) |
            static_cast<std::uint32_t>(bitmap.pixels[offset + 2]));
    }
    Check(colors.size() > 128,
        "preview stage retains a varied multicolor background");
    return bitmap;
}

void CheckPomodoroPrimaryActionCentered(const std::filesystem::path& path)
{
    const RgbaBitmap bitmap = ReadPng(path);
    const auto isTomato = [&](UINT x, UINT y) {
        const std::size_t offset =
            (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
        const int red = bitmap.pixels[offset];
        const int green = bitmap.pixels[offset + 1];
        const int blue = bitmap.pixels[offset + 2];
        const int alpha = bitmap.pixels[offset + 3];
        return alpha > 0 && red > 140 && red > green + 35 &&
            red > blue + 30;
    };

    std::vector<std::uint8_t> candidates(
        static_cast<std::size_t>(bitmap.width) * bitmap.height, 0);
    for (UINT y = 0; y < bitmap.height; ++y)
    {
        for (UINT x = 0; x < bitmap.width; ++x)
        {
            if (isTomato(x, y))
                candidates[static_cast<std::size_t>(y) * bitmap.width + x] = 1;
        }
    }

    PixelBounds primaryButton;
    std::vector<std::size_t> pending;
    for (UINT y = 0; y < bitmap.height; ++y)
    {
        for (UINT x = 0; x < bitmap.width; ++x)
        {
            const std::size_t start =
                static_cast<std::size_t>(y) * bitmap.width + x;
            if (!candidates[start]) continue;
            candidates[start] = 0;
            pending.assign(1, start);
            PixelBounds component;
            while (!pending.empty())
            {
                const std::size_t index = pending.back();
                pending.pop_back();
                const UINT currentX =
                    static_cast<UINT>(index % bitmap.width);
                const UINT currentY =
                    static_cast<UINT>(index / bitmap.width);
                component.Include(static_cast<int>(currentX),
                    static_cast<int>(currentY));
                const auto Visit = [&](UINT nextX, UINT nextY) {
                    const std::size_t next =
                        static_cast<std::size_t>(nextY) * bitmap.width + nextX;
                    if (!candidates[next]) return;
                    candidates[next] = 0;
                    pending.push_back(next);
                };
                if (currentX > 0) Visit(currentX - 1, currentY);
                if (currentX + 1 < bitmap.width)
                    Visit(currentX + 1, currentY);
                if (currentY > 0) Visit(currentX, currentY - 1);
                if (currentY + 1 < bitmap.height)
                    Visit(currentX, currentY + 1);
            }
            if (component.count > primaryButton.count &&
                component.bottom > static_cast<int>(bitmap.height / 2))
            {
                primaryButton = component;
            }
        }
    }
    const int buttonWidth = primaryButton.right - primaryButton.left + 1;
    const int buttonHeight = primaryButton.bottom - primaryButton.top + 1;
    Check(primaryButton.count > 500 &&
            buttonWidth > static_cast<int>(bitmap.width * 0.55) &&
            buttonWidth > buttonHeight * 2,
        "Pomodoro primary action surface is found in the preview");
    const double buttonCenterX =
        (primaryButton.left + primaryButton.right) * 0.5;
    Check(std::abs(buttonCenterX - (bitmap.width - 1) * 0.5) <= 2.0,
        "Pomodoro primary action is horizontally centered");

    PixelBounds label;
    for (int y = primaryButton.top; y <= primaryButton.bottom; ++y)
    {
        for (int x = primaryButton.left; x <= primaryButton.right; ++x)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
            const int red = bitmap.pixels[offset];
            const int green = bitmap.pixels[offset + 1];
            const int blue = bitmap.pixels[offset + 2];
            const int alpha = bitmap.pixels[offset + 3];
            const int darkest = std::min({ red, green, blue });
            const int lightest = std::max({ red, green, blue });
            if (alpha > 0 && darkest >= 200 && lightest - darkest <= 35)
                label.Include(x, y);
        }
    }
    Check(label.count > 40,
        "Pomodoro primary action label is found in the preview");
    const double buttonCenterY =
        (primaryButton.top + primaryButton.bottom) * 0.5;
    const double labelCenterX = (label.left + label.right) * 0.5;
    const double labelCenterY = (label.top + label.bottom) * 0.5;
    Check(std::abs(labelCenterX - buttonCenterX) <= 2.0 &&
            std::abs(labelCenterY - buttonCenterY) <= 2.0,
        "Pomodoro primary action label is visually centered");
}

void Write(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    Check(static_cast<bool>(output), "preview fixture is written");
}

std::filesystem::path CreateEnvironmentFixture(
    const std::filesystem::path& root)
{
    const auto source = root / L"environment-widget";
    std::error_code error;
    Check(std::filesystem::create_directory(source, error),
        "preview environment fixture directory is created");
    Write(source / L"widget.json", R"json({
  "schemaVersion": 2,
  "apiVersion": 2,
  "dataVersion": 1,
  "id": "86b63935-cb1e-4b97-8171-185a0cbd9fb4",
  "slug": "preview-environment-fixture",
  "version": "1.0.0",
  "entry": "main.lua",
  "minHostVersion": "1.0.1.0",
  "name": "Preview environment fixture",
  "description": "Validates the authoring preview context.",
  "author": "SnowDesktop",
  "license": "MIT",
  "defaultSize": {"columns": 2, "rows": 1},
  "permissions": ["system.performance.read"],
  "requiredFeatures": [
    "draw.immediate",
    "draw.marqueeText",
    "layout.relativeUnits",
    "lifecycle.model",
    "widget.context",
    "data.subscribe",
    "data.system.cpu"
  ]
})json");
    Write(source / L"main.lua", R"lua(
local cpu

return widget.define({
    setup = function(context)
        assert(context.preview == true, "preview flag was not injected")
        assert(context.locale == "zh-CN", "locale was not injected")
        assert(context.theme.mode == "light", "theme was not injected")
        assert(context.dpi.x == 144 and context.dpi.y == 144,
            "DPI was not injected before setup")
        assert(context.grid.columns == 2 and context.grid.rows == 1,
            "size was not injected before setup")
        assert(context.pixelSize.width == 288 and
            context.pixelSize.height == 174 and
            context.logicalSize.width == 192 and
            context.logicalSize.height == 116,
            "pixel and DPI-normalized sizes were not kept distinct")
        assert(context.layoutSize.width == layout.contentWidth() and
            context.layoutSize.height == layout.contentHeight() and
            context.layoutSize.width == 288 and
            context.layoutSize.height == 174,
            "layoutSize must match the root content coordinate space")
        assert(layout.vw(50) == 144 and layout.vh(50) == 87 and
            layout.vmin(50) == 87 and layout.vmax(50) == 144,
            "relative layout units must use the root content extents")
        assert(pcall(layout.vw, -1) == false and
            pcall(layout.vh, 101) == false,
            "relative layout units must reject out-of-range percentages")
        assert(context.monitor.available == false,
            "preview must not expose the developer monitor")
        assert(context.accessibility.highContrast == false and
            context.accessibility.reducedMotion == false and
            context.accessibility.textScale == 1,
            "preview accessibility defaults were not deterministic")
        assert(context.theme.accentColor == 0x0078D4 and
            context.region == "CN" and context.timeZone == "UTC" and
            context.utcOffsetMinutes == 0 and
            context.inputLanguage == "zh-CN",
            "preview system environment was not deterministic")
        cpu = data.subscribe("system.cpu", { maxAgeMs = 500 })
        local snapshot = cpu:value()
        assert(snapshot.available == true and snapshot.stale == true and
            snapshot.warmingUp == false and snapshot.error == nil,
            "stale preview data state was not injected")
        assert(snapshot.timestamp == 1785662998999,
            "preview data timestamp did not use the virtual clock")
        return {}
    end,
    render = function()
        draw.rect(0, 0, layout.width(), layout.height(), 0xE8EEF8, 8, 1)
        draw.text(12, 12, "preview", 16, 0x172033)
        local scrolling = draw.marqueeText({
            key = "preview.marquee",
            x = 12,
            y = 42,
            width = 48,
            height = 20,
            text = "native marquee preview",
            size = 14,
            color = 0x172033,
        })
        assert(scrolling == true,
            "native marquee must report overflowing preview text")
        assert(pcall(draw.marqueeText, {
            key = "invalid",
            x = 0,
            y = 0,
            width = 0,
            height = 20,
            text = "invalid",
        }) == false, "native marquee must reject an empty viewport")
    end,
    dispose = function()
        if cpu then cpu:unsubscribe() end
    end,
})
)lua");
    return source;
}
}

int wmain(int argc, wchar_t** argv)
{
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
        "COM initializes for preview pixel decoding");
    Check(argc == 4,
        "test receives snowwidget, SnowDesktop, and repository root");
    const std::filesystem::path snowwidget = argv[1];
    const std::filesystem::path host = argv[2];
    const std::filesystem::path repository = argv[3];
    Check(std::filesystem::is_regular_file(snowwidget),
        "snowwidget executable exists");
    Check(std::filesystem::is_regular_file(host),
        "SnowDesktop preview host exists");

    TemporaryDirectory temporary;
    const auto background = temporary.path / L"author-background.bmp";
    WriteSolidBmp(background, 18, 126, 214);
    const auto backgroundOutput = temporary.path / L"custom-background.png";
    const auto backgroundSource = repository / L"widgets" / L"analog-clock";
    const auto [backgroundExit, backgroundJson] = Run(snowwidget, {
        L"preview", backgroundSource.wstring(), backgroundOutput.wstring(),
        L"--background", background.wstring(), L"--host", host.wstring() });
    Check(backgroundExit == 0 &&
            backgroundJson.find("\"background\":") != std::string::npos &&
            backgroundJson.find("author-background.bmp") !=
                std::string::npos,
        "preview reports the developer-selected background image");
    const RgbaBitmap selectedBackground = ReadPng(backgroundOutput);
    Check(selectedBackground.pixels[0] == 18 &&
            selectedBackground.pixels[1] == 126 &&
            selectedBackground.pixels[2] == 214 &&
            selectedBackground.pixels[3] == 255,
        "transparent preview pixels reveal the selected author background");

    const auto squareOutput = temporary.path / L"square-preview.png";
    const auto [squareExit, squareJson] = Run(snowwidget, {
        L"preview", backgroundSource.wstring(), squareOutput.wstring(),
        L"--background", background.wstring(),
        L"--canvas-size", L"512", L"--padding", L"48",
        L"--host", host.wstring() });
    Check(squareExit == 0 &&
            squareJson.find("\"width\":512") != std::string::npos &&
            squareJson.find("\"height\":512") != std::string::npos &&
            squareJson.find("\"componentWidth\":192") !=
                std::string::npos &&
            squareJson.find("\"componentHeight\":240") !=
                std::string::npos &&
            squareJson.find("\"canvasSize\":512") !=
                std::string::npos &&
            squareJson.find("\"padding\":48") != std::string::npos &&
            squareJson.find("\"placementX\":89") !=
                std::string::npos &&
            squareJson.find("\"placementY\":48") !=
                std::string::npos &&
            squareJson.find("\"placementWidth\":333") !=
                std::string::npos &&
            squareJson.find("\"placementHeight\":416") !=
                std::string::npos,
        "preview reports the native component layer and square canvas placement");
    const RgbaBitmap square =
        CheckOpaquePreview(squareOutput, 512, 512);
    Check(PixelAt(square, 0, 0) ==
            std::array<std::uint8_t, 4>{ 18, 126, 214, 255 } &&
            PixelAt(square, 511, 511) ==
                std::array<std::uint8_t, 4>{ 18, 126, 214, 255 },
        "square composition preserves the background outside component padding");

    const auto output = temporary.path / L"analog-clock.png";
    const auto source = repository / L"widgets" / L"analog-clock";
    const auto [exitCode, json] = Run(snowwidget, {
        L"preview", source.wstring(), output.wstring(),
        L"--dpi", L"144", L"--storage", L"showNumbers=1",
        L"--host", host.wstring() });
    Check(exitCode == 0 &&
            json.find("\"ok\":true") != std::string::npos &&
            json.find("\"stage\":\"complete\"") != std::string::npos &&
            json.find("\"columns\":2") != std::string::npos &&
            json.find("\"rows\":2") != std::string::npos &&
            json.find("\"dpi\":144") != std::string::npos &&
            json.find("\"theme\":\"dark\"") != std::string::npos &&
            json.find("\"appearance\":\"dark\"") != std::string::npos,
        "snowwidget preview reports a completed real API v2 render");
    CheckPng(output);
    const RgbaBitmap customDark = CheckOpaquePreview(output, 288, 360);

    const auto lightForegroundOutput =
        temporary.path / L"analog-clock-light-foreground.png";
    const auto darkForegroundOutput =
        temporary.path / L"analog-clock-dark-foreground.png";
    const auto [lightForegroundExit, lightForegroundJson] = Run(snowwidget, {
        L"preview", source.wstring(), lightForegroundOutput.wstring(),
        L"--appearance", L"acrylic-light", L"--storage",
        L"followPersonalization=0", L"--storage", L"__contentTheme=0",
        L"--host", host.wstring() });
    const auto [darkForegroundExit, darkForegroundJson] = Run(snowwidget, {
        L"preview", source.wstring(), darkForegroundOutput.wstring(),
        L"--appearance", L"acrylic-light", L"--storage",
        L"followPersonalization=0", L"--storage", L"__contentTheme=1",
        L"--host", host.wstring() });
    Check(lightForegroundExit == 0 && darkForegroundExit == 0 &&
            lightForegroundJson.find("\"ok\":true") !=
                std::string::npos &&
            lightForegroundJson.find("\"contentTheme\":0") !=
                std::string::npos &&
            lightForegroundJson.find("\"foregroundTheme\":\"light\"") !=
                std::string::npos &&
            darkForegroundJson.find("\"ok\":true") !=
                std::string::npos &&
            darkForegroundJson.find("\"contentTheme\":1") !=
                std::string::npos &&
            darkForegroundJson.find("\"foregroundTheme\":\"dark\"") !=
                std::string::npos &&
            CheckOpaquePreview(lightForegroundOutput, 192, 240).pixels !=
                CheckOpaquePreview(darkForegroundOutput, 192, 240).pixels,
        "custom foreground themes render independently of preview material");

    const auto customGlassOutput =
        temporary.path / L"analog-clock-custom-glass.png";
    const auto [customGlassExit, customGlassJson] = Run(snowwidget, {
        L"preview", source.wstring(), customGlassOutput.wstring(),
        L"--dpi", L"144", L"--storage", L"showNumbers=1",
        L"--appearance", L"glass-dark", L"--host", host.wstring() });
    Check(customGlassExit == 0 &&
            customGlassJson.find("\"appearance\":\"glass-dark\"") !=
                std::string::npos &&
            CheckOpaquePreview(customGlassOutput, 288, 360).pixels ==
                customDark.pixels,
        "a component custom transparent style overrides the host material");

    const auto transparentOutput = temporary.path / L"transparent.png";
    const auto glassOutput = temporary.path / L"glass.png";
    const auto acrylicOutput = temporary.path / L"acrylic.png";
    const auto [transparentExit, transparentJson] = Run(snowwidget, {
        L"preview", source.wstring(), transparentOutput.wstring(),
        L"--appearance", L"dark", L"--storage",
        L"edgeHighlightEnabled=0", L"--host", host.wstring() });
    const auto [glassExit, glassJson] = Run(snowwidget, {
        L"preview", source.wstring(), glassOutput.wstring(),
        L"--appearance", L"dark", L"--storage", L"glassEnabled=1",
        L"--storage", L"edgeHighlightEnabled=0",
        L"--host", host.wstring() });
    const auto [acrylicExit, acrylicJson] = Run(snowwidget, {
        L"preview", source.wstring(), acrylicOutput.wstring(),
        L"--appearance", L"dark", L"--storage", L"glassEnabled=1",
        L"--storage", L"acrylicEnabled=1", L"--storage",
        L"edgeHighlightEnabled=0", L"--host", host.wstring() });
    Check(transparentExit == 0 && glassExit == 0 && acrylicExit == 0 &&
            transparentJson.find("\"ok\":true") != std::string::npos &&
            glassJson.find("\"ok\":true") != std::string::npos &&
            acrylicJson.find("\"ok\":true") != std::string::npos,
        "transparent, glass, and acrylic custom materials render");
    const RgbaBitmap transparent =
        CheckOpaquePreview(transparentOutput, 192, 240);
    const RgbaBitmap customMaterialGlass =
        CheckOpaquePreview(glassOutput, 192, 240);
    const RgbaBitmap customMaterialAcrylic =
        CheckOpaquePreview(acrylicOutput, 192, 240);
    constexpr RECT unobscuredPanelSample{ 8, 8, 184, 40 };
    Check(PixelAt(transparent, 0, 0) ==
            PixelAt(customMaterialGlass, 0, 0) &&
            PixelAt(transparent, 191, 0) ==
                PixelAt(customMaterialGlass, 191, 0) &&
            PixelAt(transparent, 0, 239) ==
                PixelAt(customMaterialGlass, 0, 239) &&
            PixelAt(transparent, 191, 239) ==
                PixelAt(customMaterialGlass, 191, 239) &&
            CountDifferingPixels(transparent, customMaterialGlass,
                unobscuredPanelSample) > 512,
        "custom glass blurs only the rounded panel interior");
    Check(customMaterialGlass.pixels != customMaterialAcrylic.pixels,
        "custom acrylic adds one stable noise layer over custom glass");

    const auto standardBorderOutput =
        temporary.path / L"border-standard.png";
    const auto zeroStrengthBorderOutput =
        temporary.path / L"edge-highlight-zero.png";
    const auto borderlessOutput =
        temporary.path / L"edge-highlight-borderless.png";
    const auto edgeHighlightOutput =
        temporary.path / L"edge-highlight-75.png";
    const auto glassEdgeHighlightOutput =
        temporary.path / L"glass-edge-highlight-75.png";
    const auto alternateHiddenBorderOutput =
        temporary.path / L"edge-highlight-hidden-border-color.png";
    const auto wideEdgeHighlightOutput =
        temporary.path / L"edge-highlight-wide.png";
    const auto [standardBorderExit, standardBorderJson] = Run(snowwidget, {
        L"preview", source.wstring(), standardBorderOutput.wstring(),
        L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
        L"--storage", L"borderAlpha=0.75",
        L"--storage", L"borderWidth=2",
        L"--storage", L"edgeHighlightEnabled=0",
        L"--storage", L"edgeHighlightStrength=0.75",
        L"--host", host.wstring() });
    const auto [zeroStrengthBorderExit, zeroStrengthBorderJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), zeroStrengthBorderOutput.wstring(),
            L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
            L"--storage", L"borderAlpha=0.75",
            L"--storage", L"borderWidth=2",
            L"--storage", L"edgeHighlightEnabled=1",
            L"--storage", L"edgeHighlightWidth=2",
            L"--storage", L"edgeHighlightStrength=0",
            L"--host", host.wstring() });
    const auto [borderlessExit, borderlessJson] = Run(snowwidget, {
        L"preview", source.wstring(), borderlessOutput.wstring(),
        L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
        L"--storage", L"borderAlpha=0",
        L"--storage", L"edgeHighlightEnabled=0",
        L"--host", host.wstring() });
    const auto [edgeHighlightExit, edgeHighlightJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), edgeHighlightOutput.wstring(),
            L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
            L"--storage", L"borderAlpha=0",
            L"--storage", L"edgeHighlightEnabled=1",
            L"--storage", L"edgeHighlightWidth=2",
            L"--storage", L"edgeHighlightStrength=0.75",
            L"--host", host.wstring() });
    const auto [glassEdgeHighlightExit, glassEdgeHighlightJson] =
        Run(snowwidget, {
            L"preview", source.wstring(),
            glassEdgeHighlightOutput.wstring(),
            L"--appearance", L"dark", L"--storage", L"glassEnabled=1",
            L"--storage", L"borderAlpha=0",
            L"--storage", L"edgeHighlightEnabled=1",
            L"--storage", L"edgeHighlightWidth=2",
            L"--storage", L"edgeHighlightStrength=0.75",
            L"--host", host.wstring() });
    const auto [wideEdgeHighlightExit, wideEdgeHighlightJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), wideEdgeHighlightOutput.wstring(),
            L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
            L"--storage", L"borderAlpha=0",
            L"--storage", L"edgeHighlightEnabled=1",
            L"--storage", L"edgeHighlightWidth=4",
            L"--storage", L"edgeHighlightStrength=0.75",
            L"--host", host.wstring() });
    const auto [alternateHiddenBorderExit, alternateHiddenBorderJson] =
        Run(snowwidget, {
            L"preview", source.wstring(),
            alternateHiddenBorderOutput.wstring(),
            L"--appearance", L"dark", L"--storage", L"glassEnabled=0",
            L"--storage", L"border=16711680",
            L"--storage", L"borderAlpha=0",
            L"--storage", L"edgeHighlightEnabled=1",
            L"--storage", L"edgeHighlightWidth=2",
            L"--storage", L"edgeHighlightStrength=0.75",
            L"--host", host.wstring() });
    Check(standardBorderExit == 0 && zeroStrengthBorderExit == 0 &&
            borderlessExit == 0 &&
            edgeHighlightExit == 0 && glassEdgeHighlightExit == 0 &&
            wideEdgeHighlightExit == 0 &&
            alternateHiddenBorderExit == 0 &&
            standardBorderJson.find("\"ok\":true") != std::string::npos &&
            zeroStrengthBorderJson.find("\"ok\":true") !=
                std::string::npos &&
            borderlessJson.find("\"ok\":true") != std::string::npos &&
            edgeHighlightJson.find("\"ok\":true") !=
                std::string::npos &&
            glassEdgeHighlightJson.find("\"ok\":true") !=
                std::string::npos &&
            wideEdgeHighlightJson.find("\"ok\":true") !=
                std::string::npos &&
            alternateHiddenBorderJson.find("\"ok\":true") !=
                std::string::npos,
        "ordinary border plus zero-strength, recommended, alternate-border, and maximum-width edge highlights render");
    const RgbaBitmap standardBorder =
        CheckOpaquePreview(standardBorderOutput, 192, 240);
    const RgbaBitmap zeroStrengthBorder =
        CheckOpaquePreview(zeroStrengthBorderOutput, 192, 240);
    const RgbaBitmap borderless =
        CheckOpaquePreview(borderlessOutput, 192, 240);
    const RgbaBitmap edgeHighlight =
        CheckOpaquePreview(edgeHighlightOutput, 192, 240);
    const RgbaBitmap glassEdgeHighlight =
        CheckOpaquePreview(glassEdgeHighlightOutput, 192, 240);
    const RgbaBitmap alternateHiddenBorder =
        CheckOpaquePreview(alternateHiddenBorderOutput, 192, 240);
    const RgbaBitmap wideEdgeHighlight =
        CheckOpaquePreview(wideEdgeHighlightOutput, 192, 240);
    Check(standardBorder.pixels == zeroStrengthBorder.pixels,
        "zero-percent edge highlight is pixel-identical to the ordinary border");
    Check(standardBorder.pixels != edgeHighlight.pixels &&
            edgeHighlight.pixels != wideEdgeHighlight.pixels,
        "recommended edge highlight and maximum width produce distinct borderless output");
    Check(edgeHighlight.pixels == alternateHiddenBorder.pixels,
        "a transparent border color does not tint the panel-material edge highlight");
    constexpr RECT wholePanel{ 0, 0, 192, 240 };
    Check(CountDarkenedPixels(borderless, edgeHighlight, wholePanel) == 0 &&
            CountDarkenedPixels(borderless, wideEdgeHighlight,
                wholePanel) == 0,
        "edge highlights only add light to the existing panel pixels");
    Check(transparent.pixels != customMaterialGlass.pixels &&
            transparent.pixels != edgeHighlight.pixels &&
            customMaterialGlass.pixels != glassEdgeHighlight.pixels &&
            edgeHighlight.pixels != glassEdgeHighlight.pixels &&
            CountDarkenedPixels(customMaterialGlass,
                glassEdgeHighlight, wholePanel) == 0,
        "glass and edge-highlight toggles render all four explicit combinations without either control changing the other");
    constexpr RECT outerTopLight{ 64, 0, 128, 1 };
    constexpr RECT nearRimTopLight{ 64, 1, 128, 2 };
    constexpr RECT softShoulderTopLight{ 64, 3, 128, 4 };
    constexpr RECT innerTopTail{ 64, 7, 128, 8 };
    constexpr RECT deepInterior{ 11, 11, 181, 229 };
    const std::uint64_t outerGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, outerTopLight);
    const std::uint64_t nearRimGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, nearRimTopLight);
    const std::uint64_t shoulderGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, softShoulderTopLight);
    const std::uint64_t tailGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, innerTopTail);
    Check(outerGain > nearRimGain && nearRimGain > shoulderGain &&
            shoulderGain > tailGain && tailGain > 0 &&
            CountDifferingPixels(borderless, wideEdgeHighlight,
                deepInterior) == 0,
        "bevel reflection peaks at the outer rim and fades monotonically through a soft shoulder before the panel interior");
    constexpr RECT topLightStrip{ 64, 0, 128, 10 };
    constexpr RECT bottomLightStrip{ 64, 230, 128, 240 };
    constexpr RECT leftLightStrip{ 0, 80, 10, 160 };
    constexpr RECT rightLightStrip{ 182, 80, 192, 160 };
    const std::uint64_t topGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, topLightStrip);
    const std::uint64_t bottomGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, bottomLightStrip);
    const std::uint64_t leftGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, leftLightStrip);
    const std::uint64_t rightGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, rightLightStrip);
    constexpr RECT topLeftCornerLight{ 0, 0, 16, 16 };
    const std::uint64_t topLeftCornerGain = SumBrightnessGain(
        borderless, wideEdgeHighlight, topLeftCornerLight);
    const std::uint64_t topChangedPixels = CountDifferingPixels(
        borderless, wideEdgeHighlight, topLightStrip);
    const std::uint64_t topLeftCornerChangedPixels = CountDifferingPixels(
        borderless, wideEdgeHighlight, topLeftCornerLight);
    Check(topGain > bottomGain * 2 && leftGain > rightGain * 2 &&
            bottomGain * 10 > topGain * 3 &&
            rightGain * 10 > leftGain * 3,
        "the crest-free opposite transmission remains visible at roughly one-third to one-half of the primary reflection");
    Check(topChangedPixels > 0 && topLeftCornerChangedPixels > 0 &&
            topLeftCornerGain * topChangedPixels * 5 <
                topGain * topLeftCornerChangedPixels * 7,
        "broad area light keeps the rounded corner from overpowering the connected straight edge");

    constexpr std::array<std::wstring_view, 6> appearances{
        L"dark", L"light", L"glass-dark", L"glass-light",
        L"acrylic-dark", L"acrylic-light" };
    constexpr std::array<std::string_view, 6> appearanceNames{
        "dark", "light", "glass-dark", "glass-light",
        "acrylic-dark", "acrylic-light" };
    constexpr std::array<std::string_view, 6> appearanceThemes{
        "dark", "light", "dark", "light", "dark", "light" };
    constexpr std::array<int, 6> appearanceContentThemes{
        0, 1, 0, 0, 0, 1 };
    std::vector<RgbaBitmap> materialPreviews;
    for (std::size_t index = 0; index < appearances.size(); ++index)
    {
        const auto materialOutput = temporary.path /
            (L"material-" + std::wstring(appearances[index]) + L".png");
        const auto [materialExit, materialJson] = Run(snowwidget, {
            L"preview", source.wstring(), materialOutput.wstring(),
            L"--appearance", std::wstring(appearances[index]),
            L"--storage", L"followPersonalization=1",
            L"--host", host.wstring() });
        const std::string expectedAppearance = "\"appearance\":\"" +
            std::string(appearanceNames[index]) + "\"";
        const std::string expectedTheme = "\"theme\":\"" +
            std::string(appearanceThemes[index]) + "\"";
        const std::string expectedContentTheme = "\"contentTheme\":" +
            std::to_string(appearanceContentThemes[index]);
        Check(materialExit == 0 &&
                materialJson.find(expectedAppearance) != std::string::npos &&
                materialJson.find(expectedTheme) != std::string::npos &&
                materialJson.find(expectedContentTheme) != std::string::npos,
            "each supported appearance reports its stage and resolved foreground identities");
        materialPreviews.push_back(
            CheckOpaquePreview(materialOutput, 192, 240));
    }
    for (std::size_t index = 1; index < materialPreviews.size(); ++index)
    {
        Check(materialPreviews[index].pixels !=
                materialPreviews[index - 1].pixels,
            "normal, glass, and acrylic material previews remain distinct");
    }
    Check(PixelAt(materialPreviews[0], 0, 0) ==
            PixelAt(materialPreviews[2], 0, 0) &&
            PixelAt(materialPreviews[0], 191, 0) ==
                PixelAt(materialPreviews[2], 191, 0) &&
            PixelAt(materialPreviews[0], 0, 239) ==
                PixelAt(materialPreviews[2], 0, 239) &&
            PixelAt(materialPreviews[0], 191, 239) ==
                PixelAt(materialPreviews[2], 191, 239) &&
            CountDifferingPixels(materialPreviews[0], materialPreviews[2],
                unobscuredPanelSample) > 512,
        "glass blur changes the panel interior without blurring its rounded corner");
    Check(materialPreviews[0].pixels != materialPreviews[2].pixels &&
            materialPreviews[2].pixels != materialPreviews[4].pixels &&
            materialPreviews[1].pixels != materialPreviews[3].pixels &&
            materialPreviews[3].pixels != materialPreviews[5].pixels,
        "normal, glass, and acrylic are distinct in both stage palettes");

    const auto repeatedAcrylicOutput =
        temporary.path / L"material-acrylic-dark-repeat.png";
    const auto [repeatedAcrylicExit, repeatedAcrylicJson] = Run(snowwidget, {
        L"preview", source.wstring(), repeatedAcrylicOutput.wstring(),
        L"--appearance", L"acrylic-dark", L"--storage",
        L"followPersonalization=1", L"--host", host.wstring() });
    Check(repeatedAcrylicExit == 0 &&
            repeatedAcrylicJson.find("\"ok\":true") != std::string::npos &&
            CheckOpaquePreview(repeatedAcrylicOutput, 192, 240).pixels ==
                materialPreviews[4].pixels,
        "acrylic preview noise and composition are deterministic");

    const auto pomodoroOutput = temporary.path / L"pomodoro.png";
    const auto pomodoroSource = repository / L"widgets" / L"pomodoro";
    const auto [pomodoroExit, pomodoroJson] = Run(snowwidget, {
        L"preview", pomodoroSource.wstring(), pomodoroOutput.wstring(),
        L"--dpi", L"96", L"--locale", L"zh-CN", L"--theme", L"dark",
        L"--host", host.wstring() });
    Check(pomodoroExit == 0 &&
            pomodoroJson.find("\"ok\":true") != std::string::npos,
        "Pomodoro paused-state preview renders successfully");
    CheckPng(pomodoroOutput);
    CheckPomodoroPrimaryActionCentered(pomodoroOutput);

    const auto environmentSource =
        CreateEnvironmentFixture(temporary.path);
    const auto environmentOutput = temporary.path / L"environment.png";
    const auto [environmentExit, environmentJson] = Run(snowwidget, {
        L"preview", environmentSource.wstring(),
        environmentOutput.wstring(), L"--dpi", L"144",
        L"--locale", L"zh-CN", L"--theme", L"light",
        L"--data-state", L"stale", L"--host", host.wstring() });
    Check(environmentExit == 0 &&
            environmentJson.find("\"ok\":true") != std::string::npos &&
            environmentJson.find("\"locale\":\"zh-CN\"") !=
                std::string::npos &&
            environmentJson.find("\"theme\":\"light\"") !=
                std::string::npos &&
            environmentJson.find("\"appearance\":\"light\"") !=
                std::string::npos &&
            environmentJson.find("\"dataState\":\"stale\"") !=
                std::string::npos &&
            std::filesystem::is_regular_file(environmentOutput),
        "preview injects locale, theme, size, DPI, and data state before setup");

    const auto invalidOutput = temporary.path / L"invalid.png";
    const auto boundedSource =
        repository / L"widgets" / L"media-controls";
    const auto [invalidExit, invalidJson] = Run(snowwidget, {
        L"preview", boundedSource.wstring(), invalidOutput.wstring(),
        L"--columns", L"8", L"--host", host.wstring() });
    Check(invalidExit != 0 &&
            invalidJson.find("\"stage\":\"request.size\"") !=
                std::string::npos &&
            !std::filesystem::exists(invalidOutput),
        "preview rejects a size outside the manifest before rendering");

    const auto [invalidStateExit, invalidStateJson] = Run(snowwidget, {
        L"preview", source.wstring(), invalidOutput.wstring(),
        L"--data-state", L"unknown", L"--host", host.wstring() });
    Check(invalidStateExit == 2 &&
            invalidStateJson.find("data state must be") !=
                std::string::npos,
        "preview rejects an unknown deterministic data state");

    const auto [invalidAppearanceExit, invalidAppearanceJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), invalidOutput.wstring(),
            L"--appearance", L"vibrant", L"--host", host.wstring() });
    Check(invalidAppearanceExit == 2 &&
            invalidAppearanceJson.find("appearance must be") !=
                std::string::npos,
        "preview rejects an unknown appearance");

    const auto [conflictExit, conflictJson] = Run(snowwidget, {
        L"preview", source.wstring(), invalidOutput.wstring(),
        L"--theme", L"dark", L"--appearance", L"light",
        L"--host", host.wstring() });
    Check(conflictExit == 2 &&
            conflictJson.find("cannot be used together") !=
                std::string::npos,
        "preview rejects simultaneous theme and appearance options");

    const auto [missingBackgroundExit, missingBackgroundJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), invalidOutput.wstring(),
            L"--background", (temporary.path / L"missing.jpg").wstring(),
            L"--host", host.wstring() });
    Check(missingBackgroundExit != 0 &&
            missingBackgroundJson.find(
                "\"stage\":\"request.background\"") !=
                std::string::npos,
        "preview rejects a missing or undecodable author background");

    const auto [paddingWithoutCanvasExit, paddingWithoutCanvasJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), invalidOutput.wstring(),
            L"--padding", L"48", L"--host", host.wstring() });
    Check(paddingWithoutCanvasExit == 2 &&
            paddingWithoutCanvasJson.find("padding requires canvas-size") !=
                std::string::npos,
        "preview rejects padding without an independent square canvas");

    const auto [oversizedPaddingExit, oversizedPaddingJson] =
        Run(snowwidget, {
            L"preview", source.wstring(), invalidOutput.wstring(),
            L"--canvas-size", L"512", L"--padding", L"256",
            L"--host", host.wstring() });
    Check(oversizedPaddingExit == 2 &&
            oversizedPaddingJson.find("positive canvas content area") !=
                std::string::npos,
        "preview rejects padding that consumes the square canvas");

    std::cout << "widget author preview CLI tests passed\n";
    CoUninitialize();
    return 0;
}
