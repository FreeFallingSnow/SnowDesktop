#include "widget_clipboard_task_executor.h"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
class ClipboardScope
{
public:
    ClipboardScope() : opened_(OpenClipboard(nullptr) != FALSE) {}
    ~ClipboardScope()
    {
        if (opened_) CloseClipboard();
    }
    explicit operator bool() const noexcept { return opened_; }

private:
    bool opened_ = false;
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

WidgetClipboardTaskRunResult DecodeClipboardBitmap(
    HBITMAP bitmap, bool preserveAlpha)
{
    using Microsoft::WRL::ComPtr;
    if (!bitmap)
        return { false, {}, {}, "clipboardReadFailed" };
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> sourceBitmap;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory ||
        FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
            preserveAlpha ? WICBitmapUseAlpha : WICBitmapIgnoreAlpha,
            &sourceBitmap)) || !sourceBitmap)
        return { false, {}, {}, "clipboardImageDecodeFailed" };

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(sourceBitmap->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > WidgetClipboardTaskExecutor::
            MaximumImageSourceDimension ||
        sourceHeight > WidgetClipboardTaskExecutor::
            MaximumImageSourceDimension)
        return { false, {}, {}, "clipboardImageDimensionsInvalid" };

    const double scale = std::min(1.0,
        static_cast<double>(WidgetClipboardTaskExecutor::
            MaximumImageOutputDimension) /
            static_cast<double>(std::max(sourceWidth, sourceHeight)));
    const UINT width = std::max<UINT>(1,
        static_cast<UINT>(std::lround(sourceWidth * scale)));
    const UINT height = std::max<UINT>(1,
        static_cast<UINT>(std::lround(sourceHeight * scale)));
    ComPtr<IWICBitmapSource> source;
    if (width != sourceWidth || height != sourceHeight)
    {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler ||
            FAILED(scaler->Initialize(sourceBitmap.Get(), width, height,
                WICBitmapInterpolationModeFant)) ||
            FAILED(scaler.As(&source)))
            return { false, {}, {}, "clipboardImageDecodeFailed" };
    }
    else if (FAILED(sourceBitmap.As(&source)))
    {
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
        HDC device = GetDC(nullptr);
        if (device)
        {
            bitmap = CreateDIBitmap(device, header, CBM_INIT,
                base + bitsOffset,
                reinterpret_cast<const BITMAPINFO*>(base), DIB_RGB_COLORS);
            ReleaseDC(nullptr, device);
        }
        if (format == CF_DIBV5 &&
            header->biSize >= sizeof(BITMAPV5HEADER))
        {
            const auto* headerV5 =
                reinterpret_cast<const BITMAPV5HEADER*>(base);
            preserveAlpha = headerV5->bV5AlphaMask != 0;
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
    for (const UINT format : { CF_DIBV5, CF_DIB })
    {
        if (!IsClipboardFormatAvailable(format)) continue;
        const auto bitmap = BitmapFromClipboardDib(format);
        if (!bitmap) continue;
        auto result = DecodeClipboardBitmap(bitmap->first, bitmap->second);
        DeleteObject(bitmap->first);
        return result;
    }
    if (IsClipboardFormatAvailable(CF_BITMAP))
    {
        return DecodeClipboardBitmap(
            static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)), false);
    }
    return { false, {}, {}, "formatUnavailable" };
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

WidgetClipboardTaskRunResult WriteText(std::string_view text)
{
    const std::wstring wide = Utf8ToWide(text);
    if (!text.empty() && wide.empty())
        return { false, {}, {}, "invalidArguments" };
    if (wide.size() >=
        std::numeric_limits<SIZE_T>::max() / sizeof(wchar_t))
        return { false, {}, {}, "clipboardTooLarge" };
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return { false, {}, {}, "clipboardWriteFailed" };
    void* target = GlobalLock(memory);
    if (!target)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    if (!wide.empty())
        std::memcpy(target, wide.data(), wide.size() * sizeof(wchar_t));
    static_cast<wchar_t*>(target)[wide.size()] = L'\0';
    GlobalUnlock(memory);

    ClipboardScope clipboard;
    if (!clipboard)
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardBusy" };
    }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory))
    {
        GlobalFree(memory);
        return { false, {}, {}, "clipboardWriteFailed" };
    }
    return { true, {}, {}, {} };
}

WidgetClipboardTaskRunResult ClearClipboardData()
{
    ClipboardScope clipboard;
    if (!clipboard) return { false, {}, {}, "clipboardBusy" };
    return EmptyClipboard()
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

bool WidgetClipboardTaskExecutor::ValidateRequest(
    const WidgetClipboardTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action)) return false;
    if (request.action == "clipboard.read")
        return (request.format == "text" || request.format == "image" ||
                request.format == "file-reference") &&
            request.text.empty();
    if (request.action == "clipboard.write")
        return request.format == "text" &&
            request.text.size() <= MaximumTextBytes &&
            request.text.find('\0') == std::string::npos;
    return request.format.empty() && request.text.empty();
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
        if (request.format == "file-reference")
            return ReadFileReferences();
        return ReadText();
    }
    if (request.action == "clipboard.write")
        return WriteText(request.text);
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
            result = runner_(request.request);
        }
        catch (...)
        {
            result = { false, {}, {}, "clipboardTaskFailed" };
        }
        if (result.ok)
        {
            const bool validText = result.format == "text" &&
                result.text.size() <= MaximumTextBytes &&
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
            if (!validText && !validImage && !validFiles &&
                request.request.action == "clipboard.read")
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
