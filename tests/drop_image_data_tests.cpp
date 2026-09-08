#include "drop_image_data.h"

#include <windows.h>
#include <objidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace
{
using snowdesktop::drop_image_data::Limits;
using snowdesktop::drop_image_data::SaveAsPng;
using snowdesktop::drop_image_data::SaveStatus;
using snowdesktop::drop_image_data::SourceFormat;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class ImageDataObject final : public IDataObject
{
public:
    ~ImageDataObject()
    {
        for (auto& entry : entries_)
        {
            if (entry.bitmap)
                DeleteObject(entry.bitmap);
        }
    }

    void AddBytes(CLIPFORMAT format, std::vector<std::uint8_t> bytes)
    {
        entries_.push_back({ format, TYMED_HGLOBAL,
            std::move(bytes), nullptr });
    }

    void AddStreamBytes(CLIPFORMAT format,
        std::vector<std::uint8_t> bytes)
    {
        entries_.push_back({ format, TYMED_ISTREAM,
            std::move(bytes), nullptr });
    }

    void AddBitmap(HBITMAP bitmap)
    {
        entries_.push_back({ CF_BITMAP, TYMED_GDI, {}, bitmap });
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDataObject)
            return E_NOINTERFACE;
        *object = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return --references_;
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!format || !medium) return E_POINTER;
        std::memset(medium, 0, sizeof(*medium));
        const Entry* entry = Find(format);
        if (!entry) return DV_E_FORMATETC;
        if (entry->tymed == TYMED_HGLOBAL)
        {
            HGLOBAL global = GlobalAlloc(
                GMEM_MOVEABLE, entry->bytes.size());
            if (!global) return E_OUTOFMEMORY;
            void* destination = GlobalLock(global);
            if (!destination)
            {
                GlobalFree(global);
                return E_OUTOFMEMORY;
            }
            std::memcpy(destination, entry->bytes.data(),
                entry->bytes.size());
            GlobalUnlock(global);
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = global;
            return S_OK;
        }

        if (entry->tymed == TYMED_ISTREAM)
        {
            HGLOBAL global = GlobalAlloc(
                GMEM_MOVEABLE, entry->bytes.size());
            if (!global) return E_OUTOFMEMORY;
            void* destination = GlobalLock(global);
            if (!destination)
            {
                GlobalFree(global);
                return E_OUTOFMEMORY;
            }
            std::memcpy(destination, entry->bytes.data(),
                entry->bytes.size());
            GlobalUnlock(global);
            IStream* stream = nullptr;
            const HRESULT created = CreateStreamOnHGlobal(
                global, TRUE, &stream);
            if (FAILED(created) || !stream)
            {
                GlobalFree(global);
                return FAILED(created) ? created : E_OUTOFMEMORY;
            }
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = stream;
            return S_OK;
        }

        HBITMAP copy = static_cast<HBITMAP>(CopyImage(entry->bitmap,
            IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        if (!copy) return E_OUTOFMEMORY;
        medium->tymed = TYMED_GDI;
        medium->hBitmap = copy;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*, STGMEDIUM*) override
    {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format) return E_POINTER;
        return Find(format) ? S_OK : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*, FORMATETC* output) override
    {
        if (!output) return E_POINTER;
        output->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD, IEnumFORMATETC**) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD,
        IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    struct Entry
    {
        CLIPFORMAT format = 0;
        DWORD tymed = TYMED_NULL;
        std::vector<std::uint8_t> bytes;
        HBITMAP bitmap = nullptr;
    };

    const Entry* Find(const FORMATETC* format) const
    {
        if (!format || format->dwAspect != DVASPECT_CONTENT ||
            format->lindex != -1)
            return nullptr;
        for (const auto& entry : entries_)
        {
            if (entry.format == format->cfFormat &&
                (entry.tymed & format->tymed) != 0)
                return &entry;
        }
        return nullptr;
    }

    std::atomic<ULONG> references_{ 1 };
    std::vector<Entry> entries_;
};

template <typename Header>
std::vector<std::uint8_t> HeaderAndPixels(
    const Header& header, std::initializer_list<std::uint8_t> pixels)
{
    std::vector<std::uint8_t> bytes(sizeof(header) + pixels.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::copy(pixels.begin(), pixels.end(),
        bytes.begin() + sizeof(header));
    return bytes;
}

std::vector<std::uint8_t> ValidDibV5()
{
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = 1;
    header.bV5Height = -1;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = 4;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    return HeaderAndPixels(header, { 0x10, 0x20, 0x30, 0xff });
}

std::vector<std::uint8_t> ValidDib()
{
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = 1;
    header.biHeight = 1;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = 4;
    return HeaderAndPixels(header, { 0x40, 0x50, 0x60, 0x00 });
}

std::vector<std::uint8_t> InvalidDib()
{
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = std::numeric_limits<LONG>::max();
    header.biHeight = std::numeric_limits<LONG>::max();
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    return HeaderAndPixels(header, {});
}

std::vector<std::uint8_t> ValidPng()
{
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xb5, 0x1c, 0x0c, 0x02,
        0x00, 0x00, 0x00, 0x0b, 0x49, 0x44, 0x41, 0x54,
        0x78, 0xda, 0x63, 0x64, 0xf8, 0x0f, 0x00, 0x01,
        0x05, 0x01, 0x01, 0x27, 0x18, 0xe3, 0x66,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82,
    };
}

HBITMAP ValidBitmap()
{
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = 1;
    information.bmiHeader.biHeight = -1;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &information,
        DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap && pixels)
    {
        const std::array<std::uint8_t, 4> color{ 0x70, 0x80, 0x90, 0xff };
        std::memcpy(pixels, color.data(), color.size());
    }
    return bitmap;
}

std::vector<std::uint8_t> ReadFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>() };
}

std::uint32_t ReadBigEndian32(
    const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        bytes[offset + 3];
}

void CheckOnePixelPng(const std::filesystem::path& path)
{
    const auto bytes = ReadFile(path);
    static constexpr std::array<std::uint8_t, 8> signature{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    Check(bytes.size() >= 24 &&
            std::equal(signature.begin(), signature.end(), bytes.begin()) &&
            ReadBigEndian32(bytes, 16) == 1 &&
            ReadBigEndian32(bytes, 20) == 1,
        "saved image must be a decodable one-pixel PNG container");
}

void TestFormatPriorityAndMalformedFallback(
    const std::filesystem::path& root)
{
    const CLIPFORMAT pngFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"PNG"));
    Check(pngFormat != 0, "the standard registered PNG format must exist");

    {
        ImageDataObject object;
        object.AddBytes(CF_DIBV5, ValidDibV5());
        object.AddBytes(CF_DIB, ValidDib());
        object.AddBytes(pngFormat, ValidPng());
        object.AddBitmap(ValidBitmap());
        Check(snowdesktop::drop_image_data::OffersImageData(&object),
            "the probe must recognize every supported image family");
        const auto path = root / L"priority-v5.png";
        const auto result = SaveAsPng(&object, path);
        Check(result && result.source == SourceFormat::DibV5 &&
                result.width == 1 && result.height == 1,
            "CF_DIBV5 must win over lower-fidelity representations");
        CheckOnePixelPng(path);
    }

    {
        ImageDataObject object;
        object.AddBytes(CF_DIBV5, InvalidDib());
        object.AddBytes(CF_DIB, ValidDib());
        object.AddBytes(pngFormat, ValidPng());
        object.AddBitmap(ValidBitmap());
        const auto result = SaveAsPng(&object, root / L"fallback-dib.png");
        Check(result && result.source == SourceFormat::Png,
            "a malformed CF_DIBV5 must fall through to lossless PNG");
    }

    {
        ImageDataObject object;
        object.AddBytes(CF_DIBV5, InvalidDib());
        object.AddBytes(CF_DIB, InvalidDib());
        object.AddBytes(pngFormat, { 0x89, 0x50, 0x4e, 0x47 });
        object.AddBitmap(ValidBitmap());
        const auto result = SaveAsPng(
            &object, root / L"fallback-bitmap.png");
        Check(result && result.source == SourceFormat::Bitmap,
            "CF_BITMAP must follow malformed encoded and DIB data");
    }

    {
        ImageDataObject object;
        object.AddBytes(CF_DIBV5, InvalidDib());
        object.AddBytes(CF_DIB, ValidDib());
        object.AddBytes(pngFormat, { 0x89, 0x50, 0x4e, 0x47 });
        object.AddBitmap(ValidBitmap());
        const auto result = SaveAsPng(
            &object, root / L"fallback-dib-after-png.png");
        Check(result && result.source == SourceFormat::Dib,
            "CF_DIB must follow malformed higher-fidelity formats");
    }
}

void TestBoundsRejectTruncationAndOversizedSources(
    const std::filesystem::path& root)
{
    {
        ImageDataObject object;
        object.AddBytes(CF_DIB, InvalidDib());
        const auto path = root / L"oversized-dib.png";
        const auto result = SaveAsPng(&object, path);
        Check(result.status == SaveStatus::DataInvalid &&
                !std::filesystem::exists(path),
            "oversized dimensions must be rejected before WIC allocation");
    }

    {
        auto truncated = ValidDib();
        truncated.pop_back();
        ImageDataObject object;
        object.AddBytes(CF_DIB, std::move(truncated));
        const auto path = root / L"truncated-dib.png";
        const auto result = SaveAsPng(&object, path);
        Check(result.status == SaveStatus::DataInvalid &&
                !std::filesystem::exists(path),
            "a DIB whose aligned scanline exceeds its HGLOBAL must fail");
    }

    {
        const CLIPFORMAT pngFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"PNG"));
        ImageDataObject object;
        object.AddBytes(pngFormat, ValidPng());
        Limits limits;
        limits.maximumInputBytes = 32;
        const auto path = root / L"oversized-png.png";
        const auto result = SaveAsPng(&object, path, limits);
        Check(result.status == SaveStatus::DataInvalid &&
                !std::filesystem::exists(path),
            "encoded PNG bytes must obey the same bounded input policy");
    }
}

void TestMimeNamedPngFormat(const std::filesystem::path& root)
{
    const CLIPFORMAT mimePngFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"image/png"));
    Check(mimePngFormat != 0,
        "the MIME-named registered PNG format must exist");
    ImageDataObject object;
    object.AddBytes(mimePngFormat, ValidPng());
    Check(snowdesktop::drop_image_data::OffersImageData(&object),
        "the capability probe must recognize image/png");
    Check(snowdesktop::drop_image_data::OffersImmediateImageData(&object),
        "an HGLOBAL image/png payload is safe for the immediate path");
    const auto result = SaveAsPng(&object, root / L"mime-png.png");
    Check(result && result.source == SourceFormat::Png,
        "image/png must decode when the conventional PNG format is absent");
}

void TestStreamBackedPngFormat(const std::filesystem::path& root)
{
    const CLIPFORMAT pngFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"PNG"));
    ImageDataObject object;
    object.AddStreamBytes(pngFormat, ValidPng());
    Check(snowdesktop::drop_image_data::OffersImageData(&object),
        "the capability probe must recognize stream-backed PNG data");
    Check(!snowdesktop::drop_image_data::OffersImmediateImageData(&object),
        "a producer-backed PNG stream is not an immediate UI-thread format");
    const auto result = SaveAsPng(&object, root / L"stream-png.png");
    Check(result && result.source == SourceFormat::Png,
        "a stream-backed PNG must be decoded and saved");

    Limits immediateOnly;
    immediateOnly.allowStreamInput = false;
    const auto blocked = SaveAsPng(
        &object, root / L"blocked-stream-png.png", immediateOnly);
    Check(blocked.status == SaveStatus::FormatUnavailable &&
            !std::filesystem::exists(root / L"blocked-stream-png.png"),
        "the immediate path must not consume a producer-backed PNG stream");
}

void TestBoundedOutputDoesNotOverwrite(
    const std::filesystem::path& root)
{
    const CLIPFORMAT pngFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"PNG"));
    ImageDataObject object;
    object.AddBytes(pngFormat, ValidPng());

    {
        Limits limits;
        limits.maximumPngBytes = 8;
        const auto path = root / L"bounded-output.png";
        const auto result = SaveAsPng(&object, path, limits);
        Check(result.status == SaveStatus::OutputFailed &&
                !std::filesystem::exists(path),
            "an encoded PNG above the output limit must not reach disk");
    }

    {
        const auto path = root / L"existing.png";
        {
            std::ofstream existing(path, std::ios::binary);
            existing << "keep";
        }
        const auto result = SaveAsPng(&object, path);
        const auto contents = ReadFile(path);
        Check(result.status == SaveStatus::OutputFailed &&
                std::string(contents.begin(), contents.end()) == "keep",
            "CREATE_NEW must preserve an existing destination verbatim");
    }
}
}

int main()
{
    const HRESULT apartment = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE,
        "COM must be available for WIC image tests");

    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopDropImageDataTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    Check(std::filesystem::create_directories(root),
        "the test output directory must be created");

    TestFormatPriorityAndMalformedFallback(root);
    TestBoundsRejectTruncationAndOversizedSources(root);
    TestMimeNamedPngFormat(root);
    TestStreamBackedPngFormat(root);
    TestBoundedOutputDoesNotOverwrite(root);

    std::filesystem::remove_all(root, error);
    Check(!error, "the image extraction test output must be removable");
    if (SUCCEEDED(apartment)) CoUninitialize();
    std::cout << "drop image data tests passed\n";
    return 0;
}
