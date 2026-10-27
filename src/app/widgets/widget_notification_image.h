#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::notification_detail
{
// A toast owns one CPU DIB, independent of the host's D2D device. Keep loading
// synchronous so a content update never presents a new message with an old image.
class Image
{
public:
    Image() = default;
    ~Image() { Reset(); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    HBITMAP Get() const { return bitmap_; }

    void Reset()
    {
        if (bitmap_) DeleteObject(bitmap_);
        bitmap_ = nullptr;
        stamp_.reset();
        path_.clear();
        contents_.clear();
        pixels_ = 0;
    }

    void Update(const std::wstring& path, int targetPixels)
    {
        if (path.empty() || targetPixels <= 0)
        {
            Reset();
            return;
        }

        // Exclude concurrent writers/replacements while checking and decoding.
        // If this or the identity query is unsupported, retain the old filename
        // decode/failure behavior and never cache an unverified resource.
        File file(path);
        const auto stamp = file.ReadStamp();
        if (bitmap_ && stamp && stamp_ == stamp && path_ == path &&
            pixels_ == targetPixels && file.SameContents(contents_))
            return;

        auto contents = stamp ? file.ReadContents(*stamp) : std::nullopt;
        Reset();
        bitmap_ = Decode(path, targetPixels,
            stamp && contents ? file.handle : INVALID_HANDLE_VALUE);
        if (bitmap_ && stamp && contents && file.ReadStamp() == stamp)
        {
            stamp_ = stamp;
            path_ = path;
            pixels_ = targetPixels;
            contents_ = std::move(*contents);
        }
    }

private:
    struct Stamp
    {
        ULONGLONG volume;
        std::array<BYTE, 16> fileId;
        LONGLONG size, written, changed;
        bool operator==(const Stamp&) const = default;
    };

    struct File
    {
        HANDLE handle;
        explicit File(const std::wstring& path)
            : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
        ~File()
        {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
        File(const File&) = delete;
        File& operator=(const File&) = delete;

        std::optional<Stamp> ReadStamp() const
        {
            FILE_ID_INFO id{};
            FILE_BASIC_INFO basic{};
            FILE_STANDARD_INFO standard{};
            if (handle == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandleEx(handle, FileIdInfo,
                    &id, sizeof(id)) ||
                !GetFileInformationByHandleEx(handle, FileBasicInfo,
                    &basic, sizeof(basic)) ||
                !GetFileInformationByHandleEx(handle, FileStandardInfo,
                    &standard, sizeof(standard)) || standard.Directory)
                return {};
            Stamp stamp{ id.VolumeSerialNumber, {},
                standard.EndOfFile.QuadPart, basic.LastWriteTime.QuadPart,
                basic.ChangeTime.QuadPart };
            std::copy(std::begin(id.FileId.Identifier),
                std::end(id.FileId.Identifier), stamp.fileId.begin());
            return stamp;
        }

        std::optional<std::vector<BYTE>> ReadContents(const Stamp& stamp) const
        {
            // Exact compressed bytes also detect in-place rewrites that restore
            // every timestamp. Large/unreadable sources still decode normally;
            // only the optional reuse snapshot is bounded, never image admission.
            constexpr DWORD MaximumIdentityBytes = 8 * 1024 * 1024;
            if (stamp.size <= 0 || stamp.size > MaximumIdentityBytes) return {};
            std::vector<BYTE> contents;
            try { contents.resize(static_cast<size_t>(stamp.size)); }
            catch (const std::bad_alloc&) { return {}; }
            DWORD read = 0;
            const bool loaded = ReadFile(handle, contents.data(),
                static_cast<DWORD>(contents.size()), &read, nullptr) &&
                read == contents.size();
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) || !loaded)
                return {};
            return contents;
        }

        bool SameContents(const std::vector<BYTE>& expected) const
        {
            // Validate a hit without allocating another source-sized buffer.
            std::array<BYTE, 16 * 1024> buffer;
            size_t offset = 0;
            bool same = true;
            while (same && offset < expected.size())
            {
                const DWORD size = static_cast<DWORD>((std::min)(
                    buffer.size(), expected.size() - offset));
                DWORD read = 0;
                same = ReadFile(handle, buffer.data(), size, &read, nullptr) &&
                    read == size && std::equal(buffer.begin(),
                        buffer.begin() + size, expected.begin() + offset);
                offset += size;
            }
            LARGE_INTEGER start{};
            return SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) && same;
        }
    };

    static HBITMAP Decode(const std::wstring& path, int targetPixels,
        HANDLE file)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return nullptr;
        const HRESULT opened = file != INVALID_HANDLE_VALUE
            ? factory->CreateDecoderFromFileHandle(
                reinterpret_cast<ULONG_PTR>(file), nullptr,
                WICDecodeMetadataCacheOnLoad, &decoder)
            : factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(opened) || FAILED(decoder->GetFrame(0, &frame)))
            return nullptr;

        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
            sourceWidth == 0 || sourceHeight == 0)
            return nullptr;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), targetPixels, targetPixels,
                WICBitmapInterpolationModeFant)))
            return nullptr;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)))
            return nullptr;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = targetPixels;
        info.bmiHeader.biHeight = -targetPixels;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const UINT stride = static_cast<UINT>(targetPixels * 4);
        if (!bitmap || !pixels || FAILED(converter->CopyPixels(nullptr, stride,
                stride * static_cast<UINT>(targetPixels),
                static_cast<BYTE*>(pixels))))
        {
            if (bitmap) DeleteObject(bitmap);
            return nullptr;
        }
        return bitmap;
    }

    HBITMAP bitmap_ = nullptr;
    std::optional<Stamp> stamp_;
    std::wstring path_;
    std::vector<BYTE> contents_;
    int pixels_ = 0;
};
}
