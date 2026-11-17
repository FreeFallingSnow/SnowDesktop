#include "widget_clipboard_task_executor.h"
#include "diagnostics/performance_trace.h"
#include "widget_filesystem_task_executor.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using Executor = WidgetClipboardTaskExecutor;
constexpr std::size_t MaximumHtmlContainerBytes = 4 * Executor::MaximumTextBytes;
constexpr std::size_t MaximumFilePathLength = 32766;

class ClipboardScope
{
public:
    explicit ClipboardScope(HWND owner = nullptr) : opened_(OpenClipboard(owner) != FALSE) {}
    ~ClipboardScope()
    {
        if (opened_) CloseClipboard();
    }
    explicit operator bool() const noexcept { return opened_; }
    bool Close()
    {
        if (!opened_) return true;
        if (!CloseClipboard()) return false;
        opened_ = false;
        return true;
    }

private:
    bool opened_ = false;
};

class ClipboardOwnerWindow
{
public:
    ClipboardOwnerWindow()
        : window_(CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr)) {}
    ~ClipboardOwnerWindow() { if (window_) DestroyWindow(window_); }
    HWND Get() const noexcept { return window_; }
private:
    HWND window_ = nullptr;
};

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length) != length)
        return {};
    return result;
}

bool IsClipboardString(std::string_view value) noexcept
{
    return value.size() <= Executor::MaximumTextBytes &&
        value.find('\0') == std::string_view::npos &&
        (value.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0) > 0);
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0 ||
        static_cast<std::size_t>(length) >
            WidgetClipboardTaskExecutor::MaximumTextBytes)
        return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) != length)
        return {};
    return result;
}

WidgetClipboardTaskRunResult ReadText()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
        return { false, {}, {}, "formatUnavailable" };
    const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return { false, {}, {}, "clipboardReadFailed" };
    const SIZE_T bytes = GlobalSize(handle);
    const SIZE_T maximumBytes =
        (WidgetClipboardTaskExecutor::MaximumTextBytes + 1) *
        sizeof(wchar_t);
    if (bytes < sizeof(wchar_t) || bytes > maximumBytes)
        return { false, {}, {}, "clipboardTooLarge" };
    const auto* value = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!value) return { false, {}, {}, "clipboardReadFailed" };
    const std::size_t capacity = bytes / sizeof(wchar_t);
    const wchar_t* end = static_cast<const wchar_t*>(
        std::wmemchr(value, L'\0', capacity));
    if (!end)
    {
        GlobalUnlock(handle);
        return { false, {}, {}, "clipboardReadFailed" };
    }
    const std::wstring_view wide(
        value, static_cast<std::size_t>(end - value));
    std::string text = WideToUtf8(wide);
    GlobalUnlock(handle);
    if (!wide.empty() && text.empty())
        return { false, {}, {}, "clipboardTooLarge" };
    return { true, "text", std::move(text), {} };
}

std::optional<std::string_view> ExtractHtmlFragment(std::string_view data)
{
    std::optional<std::size_t> start;
    std::optional<std::size_t> end;
    std::size_t cursor = 0;
    while (cursor < data.size() && (!start || !end))
    {
        const auto lineEnd = data.find_first_of("\r\n", cursor);
        if (lineEnd == std::string_view::npos || data[cursor] == '<') break;
        const auto line = data.substr(cursor, lineEnd - cursor);
        cursor = data.find_first_not_of("\r\n", lineEnd);
        if (cursor == std::string_view::npos) cursor = data.size();

        constexpr std::string_view startKey = "StartFragment:";
        constexpr std::string_view endKey = "EndFragment:";
        const bool isStart = line.starts_with(startKey);
        if (!isStart && !line.starts_with(endKey)) continue;
        const auto digits = line.substr(isStart ? startKey.size() : endKey.size());
        const auto first = digits.find_first_not_of(" \t");
        const auto last = digits.find_last_not_of(" \t");
        if (first == std::string_view::npos) return std::nullopt;
        const auto* limit = digits.data() + last + 1;
        std::size_t offset = 0;
        const auto parsed = std::from_chars(digits.data() + first, limit, offset);
        if (parsed.ec != std::errc{} || parsed.ptr != limit) return std::nullopt;
        (isStart ? start : end) = offset;
    }
    if (!start || !end || *start < cursor || *end < *start || *end > data.size())
        return std::nullopt;
    return data.substr(*start, *end - *start);
}

WidgetClipboardTaskRunResult ReadHtml()
{
    const UINT format = RegisterClipboardFormatW(L"HTML Format");
    if (!format) return { false, {}, {}, "clipboardReadFailed" };
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    if (!IsClipboardFormatAvailable(format))
        return { false, {}, {}, "formatUnavailable" };
    const HANDLE memory = GetClipboardData(format);
    if (!memory) return { false, {}, {}, "clipboardReadFailed" };
    const SIZE_T bytes = GlobalSize(memory);
    if (bytes > MaximumHtmlContainerBytes)
        return { false, {}, {}, "clipboardTooLarge" };
    std::string data(bytes, '\0');
    const auto* raw = static_cast<const char*>(GlobalLock(memory));
    if (!raw) return { false, {}, {}, "clipboardReadFailed" };
    std::memcpy(data.data(), raw, bytes);
    GlobalUnlock(memory);
    if (const auto nul = data.find('\0'); nul != std::string::npos)
        data.resize(nul);
    const auto html = ExtractHtmlFragment(data);
    if (!html) return { false, {}, {}, "clipboardReadFailed" };
    if (html->size() > Executor::MaximumTextBytes)
        return { false, {}, {}, "clipboardTooLarge" };
    if (!IsClipboardString(*html))
        return { false, {}, {}, "clipboardReadFailed" };
    return { true, "html", std::string(*html), {} };
}

WidgetClipboardTaskRunResult DecodeClipboardImage(
    IWICImagingFactory* factory, IWICBitmapSource* input)
{
    using Microsoft::WRL::ComPtr;

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(input->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > WidgetClipboardTaskExecutor::
            MaximumImageSourceDimension ||
        sourceHeight > WidgetClipboardTaskExecutor::
            MaximumImageSourceDimension ||
        static_cast<std::uint64_t>(sourceWidth) * sourceHeight > 64ull * 1024 * 1024)
        return { false, {}, {}, "clipboardImageDimensionsInvalid" };

    const double scale = std::min(1.0,
        static_cast<double>(WidgetClipboardTaskExecutor::
            MaximumImageOutputDimension) /
            static_cast<double>(std::max(sourceWidth, sourceHeight)));
    const UINT width = std::max<UINT>(1,
        static_cast<UINT>(std::lround(sourceWidth * scale)));
    const UINT height = std::max<UINT>(1,
        static_cast<UINT>(std::lround(sourceHeight * scale)));
    ComPtr<IWICBitmapSource> source(input);
    if (width != sourceWidth || height != sourceHeight)
    {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler ||
            FAILED(scaler->Initialize(input, width, height,
                WICBitmapInterpolationModeFant)) ||
            FAILED(scaler.As(&source)))
            return { false, {}, {}, "clipboardImageDecodeFailed" };
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(source.Get(),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return { false, {}, {}, "clipboardImageDecodeFailed" };

    auto pixels = std::make_shared<WidgetRuntimeImagePixels>();
    pixels->width = width;
    pixels->height = height;
    pixels->stride = width * 4;
    pixels->bgraPremultiplied.resize(
        static_cast<std::size_t>(pixels->stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, pixels->stride,
            static_cast<UINT>(pixels->bgraPremultiplied.size()),
            pixels->bgraPremultiplied.data())))
        return { false, {}, {}, "clipboardImageDecodeFailed" };

    WidgetClipboardTaskRunResult result;
    result.ok = true;
    result.format = "image";
    result.resourceToken = MakeWidgetRuntimeImageToken(
        "clipboard", *pixels);
    result.image = std::move(pixels);
    if (result.resourceToken.empty())
        return { false, {}, {}, "clipboardImageDecodeFailed" };
    return result;
}

WidgetClipboardTaskRunResult DecodeClipboardBitmap(
    HBITMAP bitmap, bool preserveAlpha)
{
    using Microsoft::WRL::ComPtr;
    if (!bitmap) return { false, {}, {}, "clipboardReadFailed" };
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
            preserveAlpha ? WICBitmapUseAlpha : WICBitmapIgnoreAlpha, &source)))
        return { false, {}, {}, "clipboardImageDecodeFailed" };
    return DecodeClipboardImage(factory.Get(), source.Get());
}

WidgetClipboardTaskRunResult ReadClipboardPng(UINT format)
{
    using Microsoft::WRL::ComPtr;
    const HANDLE memory = GetClipboardData(format);
    if (!memory) return { false, {}, {}, "clipboardReadFailed" };
    const SIZE_T size = GlobalSize(memory);
    if (size > Executor::MaximumImageBytes)
        return { false, {}, {}, "clipboardTooLarge" };
    if (!size) return { false, {}, {}, "clipboardReadFailed" };
    std::vector<BYTE> encoded(size);
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
    if (!bytes) return { false, {}, {}, "clipboardReadFailed" };
    std::memcpy(encoded.data(), bytes, size);
    GlobalUnlock(memory);

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(encoded.data(), static_cast<DWORD>(size))) ||
        FAILED(factory->CreateDecoder(GUID_ContainerFormatPng, nullptr, &decoder)) ||
        FAILED(decoder->Initialize(stream.Get(), WICDecodeMetadataCacheOnDemand)) ||
        FAILED(decoder->GetFrame(0, &frame)))
        return { false, {}, {}, "clipboardImageDecodeFailed" };
    return DecodeClipboardImage(factory.Get(), frame.Get());
}

std::optional<std::pair<HBITMAP, bool>> BitmapFromClipboardDib(
    UINT format)
{
    const HANDLE handle = GetClipboardData(format);
    if (!handle) return std::nullopt;
    const SIZE_T bytes = GlobalSize(handle);
    if (bytes < sizeof(BITMAPINFOHEADER) ||
        bytes > WidgetClipboardTaskExecutor::MaximumImageBytes)
        return std::nullopt;
    const auto* base = static_cast<const std::uint8_t*>(GlobalLock(handle));
    if (!base) return std::nullopt;
    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(base);
    const std::int64_t absoluteHeight = header->biHeight < 0
        ? -static_cast<std::int64_t>(header->biHeight)
        : static_cast<std::int64_t>(header->biHeight);
    const bool supportedCompression = header->biCompression == BI_RGB ||
        header->biCompression == BI_BITFIELDS ||
        header->biCompression == 6;
    const bool supportedBitDepth = header->biBitCount == 1 ||
        header->biBitCount == 4 || header->biBitCount == 8 ||
        header->biBitCount == 16 || header->biBitCount == 24 ||
        header->biBitCount == 32;
    bool valid = header->biSize >= sizeof(BITMAPINFOHEADER) &&
        header->biSize <= bytes && header->biWidth > 0 &&
        absoluteHeight > 0 && header->biPlanes == 1 &&
        supportedCompression && supportedBitDepth &&
        static_cast<std::uint32_t>(header->biWidth) <=
            WidgetClipboardTaskExecutor::MaximumImageSourceDimension &&
        absoluteHeight <= WidgetClipboardTaskExecutor::
            MaximumImageSourceDimension;
    std::size_t colorCount = header->biClrUsed;
    if (colorCount == 0 && header->biBitCount <= 8)
        colorCount = std::size_t{ 1 } << header->biBitCount;
    if (header->biBitCount <= 8)
        valid = valid && colorCount <=
            (std::size_t{ 1 } << header->biBitCount);
    else
        valid = valid && colorCount <= 256;
    std::size_t extraMasks = 0;
    if (header->biSize == sizeof(BITMAPINFOHEADER) &&
        (header->biCompression == BI_BITFIELDS ||
            header->biCompression == 6))
        extraMasks = header->biCompression == 6 ? 4 : 3;
    const std::size_t bitsOffset = static_cast<std::size_t>(header->biSize) +
        colorCount * sizeof(RGBQUAD) + extraMasks * sizeof(DWORD);
    const std::uint64_t rowBits =
        static_cast<std::uint64_t>(header->biWidth) * header->biBitCount;
    const std::uint64_t rowBytes = ((rowBits + 31) / 32) * 4;
    const std::uint64_t pixelBytes = rowBytes *
        static_cast<std::uint64_t>(absoluteHeight);
    valid = valid && bitsOffset <= bytes && pixelBytes <= bytes - bitsOffset;
    HBITMAP bitmap = nullptr;
    bool preserveAlpha = false;
    if (valid)
    {
        const auto* info = reinterpret_cast<const BITMAPINFO*>(base);
        const auto* headerV5 = format == CF_DIBV5 &&
                header->biSize >= sizeof(BITMAPV5HEADER)
            ? reinterpret_cast<const BITMAPV5HEADER*>(base) : nullptr;
        if (headerV5 && header->biBitCount == 32 &&
            header->biCompression == BI_BITFIELDS &&
            headerV5->bV5RedMask == 0x00ff0000 &&
            headerV5->bV5GreenMask == 0x0000ff00 &&
            headerV5->bV5BlueMask == 0x000000ff &&
            headerV5->bV5AlphaMask == 0xff000000)
        {
            // A display-compatible bitmap can discard the original alpha.
            // Copy packed BGRA into a DIB section before handing it to WIC.
            void* pixels = nullptr;
            bitmap = CreateDIBSection(nullptr, info, DIB_RGB_COLORS,
                &pixels, nullptr, 0);
            if (bitmap)
                std::memcpy(pixels, base + bitsOffset,
                    static_cast<std::size_t>(pixelBytes));
            preserveAlpha = true;
        }
        else if (HDC device = GetDC(nullptr))
        {
            bitmap = CreateDIBitmap(device, header, CBM_INIT,
                base + bitsOffset, info, DIB_RGB_COLORS);
            ReleaseDC(nullptr, device);
            preserveAlpha = headerV5 && headerV5->bV5AlphaMask != 0;
        }
    }
    GlobalUnlock(handle);
    if (!bitmap) return std::nullopt;
    return std::pair<HBITMAP, bool>{ bitmap, preserveAlpha };
}

WidgetClipboardTaskRunResult ReadImage()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    WidgetClipboardTaskRunResult result{ false, {}, {}, "formatUnavailable" };
    for (const wchar_t* name : { L"PNG", L"image/png" })
    {
        const UINT format = RegisterClipboardFormatW(name);
        if (!format || !IsClipboardFormatAvailable(format)) continue;
        result = ReadClipboardPng(format);
        if (result.ok) return result;
    }
    for (const UINT format : { CF_DIBV5, CF_DIB })
    {
        if (!IsClipboardFormatAvailable(format)) continue;
        const auto bitmap = BitmapFromClipboardDib(format);
        if (!bitmap)
        {
            if (result.error == "formatUnavailable")
                result.error = "clipboardImageDecodeFailed";
            continue;
        }
        result = DecodeClipboardBitmap(bitmap->first, bitmap->second);
        DeleteObject(bitmap->first);
        if (result.ok) return result;
    }
    if (IsClipboardFormatAvailable(CF_BITMAP))
    {
        return DecodeClipboardBitmap(
            static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)), false);
    }
    return result;
}

WidgetClipboardTaskRunResult ReadFileReferences()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    if (!IsClipboardFormatAvailable(CF_HDROP))
        return { false, {}, {}, "formatUnavailable" };
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) return { false, {}, {}, "clipboardReadFailed" };
    const UINT count = DragQueryFileW(drop, 0xffffffffu, nullptr, 0);
    if (count == 0) return { false, {}, {}, "formatUnavailable" };
    if (count > WidgetClipboardTaskExecutor::MaximumFileReferences)
        return { false, {}, {}, "clipboardTooLarge" };

    WidgetClipboardTaskRunResult result;
    result.ok = true;
    result.format = "file-reference";
    result.files.reserve(count);
    for (UINT index = 0; index < count; ++index)
    {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0 || length >= 32767)
            return { false, {}, {}, "clipboardReadFailed" };
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != length)
            return { false, {}, {}, "clipboardReadFailed" };
        path.resize(length);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return { false, {}, {}, "clipboardReferenceUnavailable" };
        result.files.push_back({ std::filesystem::path(std::move(path)),
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 });
    }
    return result;
}

WidgetClipboardTaskRunResult PublishClipboardData(UINT format,
    std::span<const std::byte> data, std::span<const std::byte> header = {})
{
    if (!format) return { false, {}, {}, "clipboardWriteFailed" };
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
        header.size() + data.size());
    if (!memory) return { false, {}, {}, "clipboardWriteFailed" };
    auto* target = static_cast<std::byte*>(GlobalLock(memory));
    if (!target)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    if (!header.empty()) std::memcpy(target, header.data(), header.size());
    std::memcpy(target + header.size(), data.data(), data.size());
    GlobalUnlock(memory);

    ClipboardOwnerWindow owner;
    if (!owner.Get())
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    ClipboardScope clipboard(owner.Get());
    if (!clipboard)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardBusy" };
    }
    if (!EmptyClipboard() || !SetClipboardData(format, memory))
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    return clipboard.Close() ? WidgetClipboardTaskRunResult{ true, {}, {}, {} }
        : WidgetClipboardTaskRunResult{ false, {}, {}, "clipboardWriteFailed" };
}

WidgetClipboardTaskRunResult WriteText(std::string_view text)
{
    const std::wstring wide = Utf8ToWide(text);
    if (!text.empty() && wide.empty())
        return { false, {}, {}, "invalidArguments" };
    return PublishClipboardData(CF_UNICODETEXT,
        std::as_bytes(std::span(wide.c_str(), wide.size() + 1)));
}

WidgetClipboardTaskRunResult WriteHtml(std::string_view html)
{
    constexpr std::string_view headerTemplate = "Version:1.0\r\n"
        "StartHTML:0000000000\r\nEndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    constexpr std::string_view prefix = "<html><body><!--StartFragment-->";
    constexpr std::string_view suffix = "<!--EndFragment--></body></html>";
    const auto startHtml = headerTemplate.size();
    const auto startFragment = startHtml + prefix.size();
    const auto endFragment = startFragment + html.size();
    const auto endHtml = endFragment + suffix.size();
    char header[headerTemplate.size() + 1]{};
    const int headerLength = std::snprintf(header, sizeof(header),
        "Version:1.0\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
        "StartFragment:%010zu\r\nEndFragment:%010zu\r\n",
        startHtml, endHtml, startFragment, endFragment);
    if (headerLength != static_cast<int>(startHtml))
        return { false, {}, {}, "clipboardWriteFailed" };
    std::string payload(header, startHtml);
    payload.reserve(endHtml);
    payload.append(prefix).append(html).append(suffix);
    return PublishClipboardData(RegisterClipboardFormatW(L"HTML Format"),
        std::as_bytes(std::span(payload.c_str(), payload.size() + 1)));
}

WidgetClipboardTaskRunResult WriteImage(const WidgetClipboardTaskRequest& request)
{
    const auto image = request.image ? request.image :
        WidgetFilesystemTaskExecutor::DecodeImageFile(request.imagePath, 2048);
    if (!image || !IsValidWidgetRuntimeImage(*image))
        return { false, {}, {}, "clipboardImageDecodeFailed" };

    // CF_DIBV5 carries straight BGRA. The renderer stores premultiplied BGRA.
    auto pixels = image->bgraPremultiplied;
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
    {
        const unsigned int alpha = pixels[offset + 3];
        if (alpha == 0 || alpha == 255) continue;
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            auto& value = pixels[offset + channel];
            value = static_cast<std::uint8_t>(std::min(255u,
                (value * 255u + alpha / 2u) / alpha));
        }
    }

    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = static_cast<LONG>(image->width);
    header.bV5Height = -static_cast<LONG>(image->height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(pixels.size());
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    return PublishClipboardData(CF_DIBV5, std::as_bytes(std::span(pixels)),
        std::as_bytes(std::span(&header, 1)));
}

WidgetClipboardTaskRunResult WriteFileReferences(
    const std::vector<std::filesystem::path>& files)
{
    std::wstring paths;
    for (const auto& file : files)
    {
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
            return { false, {}, {}, "clipboardReferenceUnavailable" };
        paths.append(file.native());
        paths.push_back(L'\0');
    }
    DROPFILES header{};
    header.pFiles = sizeof(header);
    header.fWide = TRUE;
    return PublishClipboardData(CF_HDROP,
        std::as_bytes(std::span(paths.c_str(), paths.size() + 1)),
        std::as_bytes(std::span(&header, 1)));
}

WidgetClipboardTaskRunResult ClearClipboardData()
{
    ClipboardOwnerWindow owner;
    if (!owner.Get()) return { false, {}, {}, "clipboardWriteFailed" };
    ClipboardScope clipboard(owner.Get());
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    return EmptyClipboard() && clipboard.Close()
        ? WidgetClipboardTaskRunResult{ true, {}, {}, {} }
        : WidgetClipboardTaskRunResult{
            false, {}, {}, "clipboardWriteFailed" };
}
}

WidgetClipboardTaskExecutor::WidgetClipboardTaskExecutor(
    Runner runner, NowProvider nowProvider)
    : runner_(std::move(runner)), nowProvider_(std::move(nowProvider))
{
    if (!runner_) runner_ = RunSystemAction;
    if (!nowProvider_)
        nowProvider_ = [] { return Clock::now(); };
}

WidgetClipboardTaskExecutor::~WidgetClipboardTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

WidgetClipboardTaskStartResult WidgetClipboardTaskExecutor::Start(
    std::uint64_t id, std::string instanceId,
    WidgetClipboardTaskRequest request)
{
    if (id == 0 || instanceId.empty() || !ValidateRequest(request))
        return { false, "invalidArguments" };
    const auto now = nowProvider_();
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id))
        return { false, "taskExecutorUnavailable" };
    if (const auto last = lastStarts_.find(instanceId);
        last != lastStarts_.end() && now >= last->second &&
        now - last->second < MinimumActionInterval)
        return { false, "rateLimited" };
    lastStarts_.insert_or_assign(instanceId, now);
    active_.insert(id);
    requests_.push_back(
        { id, std::move(instanceId), std::move(request) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return { true, {} };
}

bool WidgetClipboardTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (!active_.contains(id)) return false;
    canceled_.insert(id);
    condition_.notify_all();
    return true;
}

void WidgetClipboardTaskExecutor::ForgetInstance(
    std::string_view instanceId)
{
    std::scoped_lock lock(mutex_);
    lastStarts_.erase(std::string(instanceId));
}

std::vector<WidgetClipboardTaskCompletion>
WidgetClipboardTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetClipboardTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

bool WidgetClipboardTaskExecutor::SupportsAction(
    std::string_view action) noexcept
{
    return action == "clipboard.read" ||
        action == "clipboard.write" || action == "clipboard.clear";
}

bool WidgetClipboardTaskExecutor::SupportsFormat(std::string_view format) noexcept
{
    return format == "text" || format == "html" || format == "image" ||
        format == "file-reference";
}

bool WidgetClipboardTaskExecutor::ValidateRequest(
    const WidgetClipboardTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action)) return false;
    const bool hasImage = request.image || !request.imagePath.empty();
    const bool hasFiles = !request.files.empty();
    if (request.action != "clipboard.write")
        return !hasImage && !hasFiles && request.text.empty() &&
            (request.action == "clipboard.read"
                ? SupportsFormat(request.format) : request.format.empty());
    if (request.format == "text" || request.format == "html")
        return !hasImage && !hasFiles && IsClipboardString(request.text);
    if (request.format == "image")
        return !hasFiles && request.text.empty() &&
            (request.image ? request.imagePath.empty() &&
                IsValidWidgetRuntimeImage(*request.image)
                : request.imagePath.is_absolute());
    return request.format == "file-reference" && !hasImage &&
        request.text.empty() && hasFiles &&
        request.files.size() <= MaximumFileReferences &&
        std::all_of(request.files.begin(), request.files.end(),
            [](const auto& path) {
                return path.is_absolute() &&
                    path.native().size() <= MaximumFilePathLength &&
                    path.native().find(L'\0') == std::wstring::npos;
            });
}

WidgetClipboardTaskRunResult
WidgetClipboardTaskExecutor::RunSystemAction(
    const WidgetClipboardTaskRequest& request)
{
    if (!ValidateRequest(request))
        return { false, {}, {}, "invalidArguments" };
    if (request.action == "clipboard.read")
    {
        if (request.format == "image") return ReadImage();
        if (request.format == "html") return ReadHtml();
        if (request.format == "file-reference")
            return ReadFileReferences();
        return ReadText();
    }
    if (request.action == "clipboard.write")
    {
        if (request.format == "image") return WriteImage(request);
        if (request.format == "html") return WriteHtml(request.text);
        if (request.format == "file-reference") return WriteFileReferences(request.files);
        return WriteText(request.text);
    }
    return ClearClipboardData();
}

void WidgetClipboardTaskExecutor::WorkerMain(
    std::stop_token stopToken)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!stopToken.stop_requested())
    {
        QueuedRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.contains(request.id))
            {
                active_.erase(request.id);
                canceled_.erase(request.id);
                completions_.push_back({ request.id,
                    request.request.action, false, {}, {}, "canceled" });
                continue;
            }
        }

        WidgetClipboardTaskRunResult result;
        try
        {
            performance::TaskExecution trace(request.id, "clipboard");
            result = runner_(request.request);
        }
        catch (...)
        {
            result = { false, {}, {}, "clipboardTaskFailed" };
        }
        if (result.ok && request.request.action == "clipboard.read")
        {
            const bool validText = (result.format == "text" || result.format == "html") &&
                IsClipboardString(result.text) &&
                result.resourceToken.empty() && !result.image &&
                result.files.empty();
            const bool validImage = result.format == "image" &&
                result.text.empty() && result.image &&
                IsValidWidgetRuntimeImage(*result.image,
                    MaximumImageOutputDimension) &&
                IsWidgetRuntimeImageToken(result.resourceToken) &&
                result.files.empty();
            const bool validFiles = result.format == "file-reference" &&
                result.text.empty() && !result.image &&
                result.resourceToken.empty() && !result.files.empty() &&
                result.files.size() <= MaximumFileReferences &&
                std::all_of(result.files.begin(), result.files.end(),
                    [](const WidgetClipboardFileReference& reference) {
                        return !reference.path.empty() &&
                            reference.path.is_absolute();
                    });
            if (result.format != request.request.format ||
                (!validText && !validImage && !validFiles))
            {
                result = { false, {}, {}, "clipboardTaskFailed" };
            }
        }
        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
                result = { false, {}, {}, "canceled" };
            active_.erase(request.id);
            WidgetClipboardTaskCompletion completion;
            completion.id = request.id;
            completion.action = std::move(request.request.action);
            completion.ok = result.ok;
            completion.format = std::move(result.format);
            completion.text = std::move(result.text);
            completion.error = std::move(result.error);
            completion.resourceToken = std::move(result.resourceToken);
            completion.image = std::move(result.image);
            completion.files = std::move(result.files);
            completions_.push_back(std::move(completion));
        }
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}
}
