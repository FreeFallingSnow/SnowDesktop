#include "drop_image_data.h"

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace snowdesktop::drop_image_data
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::array<std::uint8_t, 8> PngSignature{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
};

class StgMediumScope
{
public:
    explicit StgMediumScope(STGMEDIUM& medium) noexcept
        : medium_(medium)
    {
    }

    ~StgMediumScope()
    {
        ReleaseStgMedium(&medium_);
    }

    StgMediumScope(const StgMediumScope&) = delete;
    StgMediumScope& operator=(const StgMediumScope&) = delete;

private:
    STGMEDIUM& medium_;
};

enum class AttemptState
{
    Unavailable,
    Invalid,
    Decoded,
};

struct DecodedImage
{
    AttemptState state = AttemptState::Unavailable;
    ComPtr<IWICBitmapSource> bitmap;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::array<CLIPFORMAT, 2> PngClipboardFormats() noexcept
{
    static const std::array<CLIPFORMAT, 2> formats{
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")),
        static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"image/png")),
    };
    return formats;
}

bool IsFormatOffered(IDataObject* dataObject, CLIPFORMAT format,
    DWORD tymed) noexcept
{
    if (!dataObject || format == 0) return false;
    FORMATETC request{};
    request.cfFormat = format;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = tymed;
    return dataObject->QueryGetData(&request) == S_OK;
}

bool AddWithin(std::size_t left, std::size_t right,
    std::size_t limit, std::size_t& sum) noexcept
{
    if (left > limit || right > limit - left) return false;
    sum = left + right;
    return true;
}

bool MultiplyWithin(std::uint64_t left, std::uint64_t right,
    std::uint64_t limit, std::uint64_t& product) noexcept
{
    if (left != 0 && right > limit / left) return false;
    product = left * right;
    return product <= limit;
}

bool CopyHGlobal(HGLOBAL global, std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes)
{
    if (!global) return false;
    const SIZE_T size = GlobalSize(global);
    if (size == 0 || size > maximumBytes ||
        size > static_cast<SIZE_T>(
            std::numeric_limits<DWORD>::max()))
        return false;
    const void* source = GlobalLock(global);
    if (!source) return false;
    try
    {
        const auto* begin = static_cast<const std::uint8_t*>(source);
        bytes.assign(begin, begin + size);
    }
    catch (...)
    {
        GlobalUnlock(global);
        throw;
    }
    GlobalUnlock(global);
    return true;
}

bool ReadStream(IStream* stream, std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes)
{
    if (!stream || maximumBytes == 0) return false;

    LARGE_INTEGER beginning{};
    (void)stream->Seek(beginning, STREAM_SEEK_SET, nullptr);

    STATSTG statistics{};
    if (SUCCEEDED(stream->Stat(&statistics, STATFLAG_NONAME)) &&
        statistics.cbSize.QuadPart > maximumBytes)
        return false;

    std::array<std::uint8_t, 64 * 1024> chunk{};
    for (;;)
    {
        ULONG read = 0;
        const HRESULT result = stream->Read(
            chunk.data(), static_cast<ULONG>(chunk.size()), &read);
        if (FAILED(result)) return false;
        if (read == 0) break;
        if (read > maximumBytes ||
            bytes.size() > maximumBytes - read)
            return false;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read);
        if (result == S_FALSE) break;
    }
    return !bytes.empty();
}

bool IsKnownDibHeaderSize(DWORD size) noexcept
{
    return size == sizeof(BITMAPINFOHEADER) || size == 52 || size == 56 ||
        size == sizeof(BITMAPV4HEADER) || size == sizeof(BITMAPV5HEADER);
}

bool IsSupportedBitDepth(WORD bitCount) noexcept
{
    return bitCount == 1 || bitCount == 4 || bitCount == 8 ||
        bitCount == 16 || bitCount == 24 || bitCount == 32;
}

bool ReadDword(const std::vector<std::uint8_t>& bytes,
    std::size_t offset, DWORD& value) noexcept
{
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset)
        return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return true;
}

bool ValidateBitMasks(const std::vector<std::uint8_t>& bytes,
    const BITMAPINFOHEADER& header, std::size_t maskOffset,
    std::size_t maskCount) noexcept
{
    if (maskCount < 3 || maskCount > 4) return false;
    std::array<DWORD, 4> masks{};
    for (std::size_t index = 0; index < maskCount; ++index)
    {
        if (!ReadDword(bytes, maskOffset + index * sizeof(DWORD),
                masks[index]))
            return false;
    }
    if (masks[0] == 0 || masks[1] == 0 || masks[2] == 0)
        return false;
    for (std::size_t left = 0; left < maskCount; ++left)
    {
        if (left == 3 && masks[left] == 0) return false;
        for (std::size_t right = left + 1; right < maskCount; ++right)
        {
            if ((masks[left] & masks[right]) != 0)
                return false;
        }
    }
    const DWORD allowedBits = header.biBitCount == 32
        ? std::numeric_limits<DWORD>::max()
        : (DWORD{ 1 } << header.biBitCount) - 1;
    return std::all_of(masks.begin(), masks.begin() + maskCount,
        [allowedBits](DWORD mask) { return (mask & ~allowedBits) == 0; });
}

struct DibLayout
{
    std::size_t bitsOffset = 0;
    std::uint64_t pixelBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::optional<DibLayout> ValidateDib(
    const std::vector<std::uint8_t>& bytes, bool requireV5,
    const Limits& limits) noexcept
{
    if (bytes.size() < sizeof(BITMAPINFOHEADER) ||
        bytes.size() > limits.maximumInputBytes)
        return std::nullopt;

    BITMAPINFOHEADER header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (!IsKnownDibHeaderSize(header.biSize) ||
        header.biSize > bytes.size() ||
        (requireV5 && header.biSize != sizeof(BITMAPV5HEADER)) ||
        header.biWidth <= 0 || header.biHeight == 0 ||
        header.biHeight == std::numeric_limits<LONG>::min() ||
        header.biPlanes != 1 || !IsSupportedBitDepth(header.biBitCount) ||
        (header.biCompression != BI_RGB &&
            header.biCompression != BI_BITFIELDS &&
            header.biCompression != 6))
        return std::nullopt;

    if ((header.biCompression == BI_BITFIELDS ||
            header.biCompression == 6) &&
        header.biBitCount != 16 && header.biBitCount != 32)
        return std::nullopt;
    if (header.biCompression == 6 && header.biSize < 56)
        return std::nullopt;
    if (header.biHeight < 0 && header.biCompression == 6)
        return std::nullopt;

    const auto width = static_cast<std::uint32_t>(header.biWidth);
    const std::uint32_t height = static_cast<std::uint32_t>(
        header.biHeight < 0 ? -header.biHeight : header.biHeight);
    std::uint64_t pixelCount = 0;
    if (width > limits.maximumDimension ||
        height > limits.maximumDimension ||
        !MultiplyWithin(width, height, limits.maximumPixelCount,
            pixelCount))
        return std::nullopt;

    std::size_t paletteCount = header.biClrUsed;
    if (header.biBitCount <= 8)
    {
        const std::size_t maximumPalette =
            std::size_t{ 1 } << header.biBitCount;
        if (paletteCount == 0) paletteCount = maximumPalette;
        if (paletteCount > maximumPalette) return std::nullopt;
    }
    else if (paletteCount > 256)
    {
        return std::nullopt;
    }

    std::size_t externalMaskCount = 0;
    if (header.biSize == sizeof(BITMAPINFOHEADER) &&
        (header.biCompression == BI_BITFIELDS ||
            header.biCompression == 6))
        externalMaskCount = header.biCompression == 6 ? 4 : 3;

    if (header.biCompression == BI_BITFIELDS ||
        header.biCompression == 6)
    {
        const std::size_t maskOffset = header.biSize ==
                sizeof(BITMAPINFOHEADER)
            ? sizeof(BITMAPINFOHEADER)
            : 40;
        const std::size_t maskCount = header.biCompression == 6 ? 4 : 3;
        if (!ValidateBitMasks(bytes, header, maskOffset, maskCount))
            return std::nullopt;
    }

    std::size_t bitsOffset = static_cast<std::size_t>(header.biSize);
    std::uint64_t masksBytes64 = 0;
    if (!MultiplyWithin(externalMaskCount, sizeof(DWORD),
            std::numeric_limits<std::size_t>::max(), masksBytes64) ||
        masksBytes64 > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    const auto masksBytes = static_cast<std::size_t>(masksBytes64);
    if (!AddWithin(bitsOffset, masksBytes, bytes.size(), bitsOffset))
        return std::nullopt;
    std::uint64_t paletteBytes64 = 0;
    if (!MultiplyWithin(paletteCount, sizeof(RGBQUAD), bytes.size(),
            paletteBytes64) ||
        paletteBytes64 > std::numeric_limits<std::size_t>::max() ||
        !AddWithin(bitsOffset, static_cast<std::size_t>(paletteBytes64),
            bytes.size(), bitsOffset))
        return std::nullopt;

    const std::uint64_t rowBits =
        static_cast<std::uint64_t>(width) * header.biBitCount;
    const std::uint64_t rowBytes = ((rowBits + 31) / 32) * 4;
    std::uint64_t pixelBytes = 0;
    if (!MultiplyWithin(rowBytes, height,
            std::numeric_limits<std::uint64_t>::max(), pixelBytes) ||
        bitsOffset > bytes.size() || pixelBytes > bytes.size() - bitsOffset)
        return std::nullopt;
    if (header.biSizeImage != 0 &&
        (header.biSizeImage < pixelBytes ||
            header.biSizeImage > bytes.size() - bitsOffset))
        return std::nullopt;

    return DibLayout{ bitsOffset, pixelBytes, width, height };
}

DecodedImage MaterializeBitmap(IWICImagingFactory* factory,
    IWICBitmapSource* source, const Limits& limits)
{
    DecodedImage result;
    result.state = AttemptState::Invalid;
    if (!factory || !source) return result;

    UINT width = 0;
    UINT height = 0;
    std::uint64_t pixelCount = 0;
    if (FAILED(source->GetSize(&width, &height)) || width == 0 ||
        height == 0 || width > limits.maximumDimension ||
        height > limits.maximumDimension ||
        !MultiplyWithin(width, height, limits.maximumPixelCount,
            pixelCount))
        return result;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(source,
            GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return result;

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromSource(converter.Get(),
            WICBitmapCacheOnLoad, &bitmap)) || !bitmap ||
        FAILED(bitmap.As(&result.bitmap)) || !result.bitmap)
        return result;

    result.state = AttemptState::Decoded;
    result.width = width;
    result.height = height;
    return result;
}

DecodedImage DecodeEncodedImage(IWICImagingFactory* factory,
    std::vector<std::uint8_t> bytes, REFGUID expectedContainer,
    std::size_t maximumEncodedBytes, const Limits& limits)
{
    DecodedImage result;
    result.state = AttemptState::Invalid;
    if (!factory || bytes.empty() ||
        bytes.size() > maximumEncodedBytes ||
        bytes.size() > std::numeric_limits<DWORD>::max())
        return result;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateStream(&stream)) || !stream ||
        FAILED(stream->InitializeFromMemory(bytes.data(),
            static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
        return result;

    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container)) ||
        container != expectedContainer)
        return result;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return result;
    return MaterializeBitmap(factory, frame.Get(), limits);
}

DecodedImage DecodeDib(IWICImagingFactory* factory,
    std::vector<std::uint8_t> dib, bool requireV5,
    const Limits& limits)
{
    const auto layout = ValidateDib(dib, requireV5, limits);
    if (!layout) return { AttemptState::Invalid };

    if (dib.size() >= sizeof(BITMAPV5HEADER))
    {
        BITMAPV5HEADER header{};
        std::memcpy(&header, dib.data(), sizeof(header));
        if (header.bV5Size == sizeof(BITMAPV5HEADER) &&
            (header.bV5CSType == PROFILE_LINKED ||
                header.bV5CSType == PROFILE_EMBEDDED))
        {
            // A linked profile may name a local or network path. Color
            // profiles are not needed to materialize a dropped image, so do
            // not let an untrusted IDataObject trigger external profile I/O.
            header.bV5CSType = LCS_sRGB;
            header.bV5ProfileData = 0;
            header.bV5ProfileSize = 0;
            std::memcpy(dib.data(), &header, sizeof(header));
        }
    }

    std::vector<std::uint8_t> bitmapFile;
    if (dib.size() > std::numeric_limits<DWORD>::max() -
            sizeof(BITMAPFILEHEADER))
        return { AttemptState::Invalid };
    bitmapFile.resize(sizeof(BITMAPFILEHEADER) + dib.size());
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfSize = static_cast<DWORD>(bitmapFile.size());
    fileHeader.bfOffBits = static_cast<DWORD>(
        sizeof(BITMAPFILEHEADER) + layout->bitsOffset);
    std::memcpy(bitmapFile.data(), &fileHeader, sizeof(fileHeader));
    std::memcpy(bitmapFile.data() + sizeof(fileHeader),
        dib.data(), dib.size());
    const std::size_t bitmapFileBytes = bitmapFile.size();
    return DecodeEncodedImage(factory, std::move(bitmapFile),
        GUID_ContainerFormatBmp, bitmapFileBytes, limits);
}

DecodedImage TryDib(IDataObject* dataObject,
    IWICImagingFactory* factory, CLIPFORMAT format, bool requireV5,
    const Limits& limits)
{
    const bool offered = IsFormatOffered(
        dataObject, format, TYMED_HGLOBAL);
    FORMATETC request{};
    request.cfFormat = format;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&request, &medium)))
        return { offered ? AttemptState::Invalid :
            AttemptState::Unavailable };
    StgMediumScope release(medium);
    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal)
        return { AttemptState::Invalid };
    std::vector<std::uint8_t> bytes;
    if (!CopyHGlobal(medium.hGlobal, limits.maximumInputBytes, bytes))
        return { AttemptState::Invalid };
    return DecodeDib(factory, std::move(bytes), requireV5, limits);
}

DecodedImage TryPngFormat(IDataObject* dataObject,
    IWICImagingFactory* factory, CLIPFORMAT format,
    const Limits& limits)
{
    if (format == 0) return {};
    const DWORD allowedTymed = TYMED_HGLOBAL |
        (limits.allowStreamInput ? TYMED_ISTREAM : 0);
    const bool offered = IsFormatOffered(
        dataObject, format, allowedTymed);
    FORMATETC request{};
    request.cfFormat = format;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = allowedTymed;
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&request, &medium)))
        return { offered ? AttemptState::Invalid :
            AttemptState::Unavailable };
    StgMediumScope release(medium);

    std::vector<std::uint8_t> bytes;
    const bool copied = medium.tymed == TYMED_HGLOBAL
        ? CopyHGlobal(medium.hGlobal, limits.maximumInputBytes, bytes)
        : limits.allowStreamInput && medium.tymed == TYMED_ISTREAM
            ? ReadStream(medium.pstm, limits.maximumInputBytes, bytes)
            : false;
    if (!copied || bytes.size() < PngSignature.size() ||
        !std::equal(PngSignature.begin(), PngSignature.end(), bytes.begin()))
        return { AttemptState::Invalid };
    return DecodeEncodedImage(factory, std::move(bytes),
        GUID_ContainerFormatPng, limits.maximumInputBytes, limits);
}

DecodedImage TryPng(IDataObject* dataObject,
    IWICImagingFactory* factory, const Limits& limits)
{
    bool encounteredInvalid = false;
    for (const CLIPFORMAT format : PngClipboardFormats())
    {
        DecodedImage decoded = TryPngFormat(
            dataObject, factory, format, limits);
        if (decoded.state == AttemptState::Decoded)
            return decoded;
        if (decoded.state == AttemptState::Invalid)
            encounteredInvalid = true;
    }
    return { encounteredInvalid ? AttemptState::Invalid :
        AttemptState::Unavailable };
}

DecodedImage TryBitmap(IDataObject* dataObject,
    IWICImagingFactory* factory, const Limits& limits)
{
    const bool offered = IsFormatOffered(
        dataObject, CF_BITMAP, TYMED_GDI);
    FORMATETC request{};
    request.cfFormat = CF_BITMAP;
    request.dwAspect = DVASPECT_CONTENT;
    request.lindex = -1;
    request.tymed = TYMED_GDI;
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&request, &medium)))
        return { offered ? AttemptState::Invalid :
            AttemptState::Unavailable };
    StgMediumScope release(medium);
    if (medium.tymed != TYMED_GDI || !medium.hBitmap)
        return { AttemptState::Invalid };

    BITMAP description{};
    if (GetObjectW(medium.hBitmap, sizeof(description), &description) !=
            static_cast<int>(sizeof(description)) ||
        description.bmWidth <= 0 || description.bmHeight <= 0)
        return { AttemptState::Invalid };

    std::uint64_t pixelCount = 0;
    const auto width = static_cast<std::uint32_t>(description.bmWidth);
    const auto height = static_cast<std::uint32_t>(description.bmHeight);
    if (width > limits.maximumDimension ||
        height > limits.maximumDimension ||
        !MultiplyWithin(width, height, limits.maximumPixelCount,
            pixelCount))
        return { AttemptState::Invalid };

    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHBITMAP(medium.hBitmap, nullptr,
            WICBitmapIgnoreAlpha, &bitmap)) || !bitmap)
        return { AttemptState::Invalid };
    return MaterializeBitmap(factory, bitmap.Get(), limits);
}

bool EncodePng(IWICImagingFactory* factory, IWICBitmapSource* source,
    const std::filesystem::path& destination,
    std::size_t maximumBytes)
{
    if (!factory || !source || destination.empty() || maximumBytes == 0)
        return false;

    ComPtr<IStream> memoryStream;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memoryStream)) ||
        !memoryStream || FAILED(factory->CreateStream(&stream)) || !stream ||
        FAILED(stream->InitializeFromIStream(memoryStream.Get())) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
            &encoder)) || !encoder ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || !frame ||
        FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->WriteSource(source, nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()) ||
        FAILED(memoryStream->Commit(STGC_DEFAULT)))
        return false;

    STATSTG statistics{};
    if (FAILED(memoryStream->Stat(&statistics, STATFLAG_NONAME)) ||
        statistics.cbSize.QuadPart == 0 ||
        statistics.cbSize.QuadPart > maximumBytes ||
        statistics.cbSize.QuadPart >
            std::numeric_limits<std::size_t>::max())
        return false;
    const std::size_t byteCount =
        static_cast<std::size_t>(statistics.cbSize.QuadPart);

    HGLOBAL global = nullptr;
    if (FAILED(GetHGlobalFromStream(memoryStream.Get(), &global)) || !global ||
        GlobalSize(global) < byteCount)
        return false;
    const void* encoded = GlobalLock(global);
    if (!encoded) return false;

    HANDLE output = CreateFileW(destination.c_str(),
        GENERIC_WRITE | DELETE, 0,
        nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (output == INVALID_HANDLE_VALUE)
    {
        GlobalUnlock(global);
        return false;
    }

    bool written = true;
    std::size_t offset = 0;
    while (offset < byteCount)
    {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            byteCount - offset, std::numeric_limits<DWORD>::max()));
        DWORD completed = 0;
        if (!WriteFile(output,
                static_cast<const std::uint8_t*>(encoded) + offset,
                requested, &completed, nullptr) || completed == 0)
        {
            written = false;
            break;
        }
        offset += completed;
    }
    if (written) written = FlushFileBuffers(output) != FALSE;
    if (written)
    {
        FILE_BASIC_INFO basicInformation{};
        basicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        written = SetFileInformationByHandle(output, FileBasicInfo,
            &basicInformation, sizeof(basicInformation)) != FALSE;
    }
    if (!written)
    {
        FILE_DISPOSITION_INFO disposition{ TRUE };
        if (!SetFileInformationByHandle(output, FileDispositionInfo,
                &disposition, sizeof(disposition)))
        {
            LARGE_INTEGER beginning{};
            if (SetFilePointerEx(output, beginning, nullptr, FILE_BEGIN))
                (void)SetEndOfFile(output);
        }
    }
    CloseHandle(output);
    GlobalUnlock(global);
    return written;
}
}

bool OffersImageData(IDataObject* dataObject) noexcept
{
    if (!dataObject) return false;
    const auto pngFormats = PngClipboardFormats();
    return IsFormatOffered(dataObject, CF_DIBV5, TYMED_HGLOBAL) ||
        IsFormatOffered(dataObject, CF_DIB, TYMED_HGLOBAL) ||
        std::any_of(pngFormats.begin(), pngFormats.end(),
            [dataObject](CLIPFORMAT png) {
                return png != 0 && IsFormatOffered(dataObject, png,
                    TYMED_HGLOBAL | TYMED_ISTREAM);
            }) ||
        IsFormatOffered(dataObject, CF_BITMAP, TYMED_GDI);
}

bool OffersImmediateImageData(IDataObject* dataObject) noexcept
{
    if (!dataObject) return false;
    const auto pngFormats = PngClipboardFormats();
    return IsFormatOffered(dataObject, CF_DIBV5, TYMED_HGLOBAL) ||
        IsFormatOffered(dataObject, CF_DIB, TYMED_HGLOBAL) ||
        std::any_of(pngFormats.begin(), pngFormats.end(),
            [dataObject](CLIPFORMAT png) {
                return png != 0 && IsFormatOffered(
                    dataObject, png, TYMED_HGLOBAL);
            }) ||
        IsFormatOffered(dataObject, CF_BITMAP, TYMED_GDI);
}

SaveResult SaveAsPng(IDataObject* dataObject,
    const std::filesystem::path& destination,
    const Limits& limits) noexcept
{
    if (!dataObject || destination.empty() ||
        limits.maximumInputBytes == 0 ||
        limits.maximumDimension == 0 || limits.maximumPixelCount == 0 ||
        limits.maximumPngBytes < PngSignature.size())
        return { SaveStatus::DataInvalid };

    try
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory)
            return { SaveStatus::DataInvalid };

        bool encounteredInvalid = false;
        const std::array<SourceFormat, 4> formats{
            SourceFormat::DibV5,
            SourceFormat::Png,
            SourceFormat::Dib,
            SourceFormat::Bitmap,
        };
        for (const SourceFormat format : formats)
        {
            DecodedImage decoded;
            switch (format)
            {
            case SourceFormat::DibV5:
                decoded = TryDib(dataObject, factory.Get(),
                    CF_DIBV5, true, limits);
                break;
            case SourceFormat::Dib:
                decoded = TryDib(dataObject, factory.Get(),
                    CF_DIB, false, limits);
                break;
            case SourceFormat::Png:
                decoded = TryPng(dataObject, factory.Get(), limits);
                break;
            case SourceFormat::Bitmap:
                decoded = TryBitmap(dataObject, factory.Get(), limits);
                break;
            default:
                break;
            }
            if (decoded.state == AttemptState::Unavailable)
                continue;
            if (decoded.state != AttemptState::Decoded || !decoded.bitmap)
            {
                encounteredInvalid = true;
                continue;
            }
            if (!EncodePng(factory.Get(), decoded.bitmap.Get(),
                    destination, limits.maximumPngBytes))
                return { SaveStatus::OutputFailed, format,
                    decoded.width, decoded.height };
            return { SaveStatus::Saved, format,
                decoded.width, decoded.height };
        }
        return { encounteredInvalid ? SaveStatus::DataInvalid :
            SaveStatus::FormatUnavailable };
    }
    catch (...)
    {
        return { SaveStatus::DataInvalid };
    }
}
}
