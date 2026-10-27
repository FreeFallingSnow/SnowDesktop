#pragma once

#include "app/widgets/widget_notification_image.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace notification_image_tests
{
inline void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Directory
{
    std::filesystem::path path;
    ~Directory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

inline void WriteBitmap(const std::filesystem::path& path, std::uint32_t color)
{
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = 1;
    info.biHeight = 1;
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = 4;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(&color), sizeof(color));
    Check(stream.good(), "notification fixture bitmap must be written");
}

inline std::vector<BYTE> Pixels(HBITMAP bitmap, int size)
{
    DIBSECTION dib{};
    Check(bitmap && GetObjectW(bitmap, sizeof(dib), &dib) == static_cast<int>(sizeof(dib)) &&
            dib.dsBm.bmWidth == size && dib.dsBm.bmHeight == size &&
            dib.dsBm.bmBits && dib.dsBm.bmBitsPixel == 32,
        "notification image must be a DIB with the requested pixel dimensions");
    const auto* begin = static_cast<const BYTE*>(dib.dsBm.bmBits);
    return { begin, begin + static_cast<size_t>(size) * size * 4 };
}

inline void Solid(HBITMAP bitmap, int size, std::uint32_t color)
{
    const auto pixels = Pixels(bitmap, size);
    for (size_t i = 0; i < pixels.size(); ++i)
        Check(pixels[i] == static_cast<BYTE>(color >> ((i % 4) * 8)),
            "notification update must show the current file's opaque BGRA pixels");
}

// Original filename-decoder pipeline, retained as the equivalence oracle for
// non-square/alpha PNG, resized pixels and malformed inputs, not cache policy.
inline std::vector<BYTE> FilenamePixels(const std::wstring& path, int size)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), size, size,
                WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)),
        "original notification filename pipeline must decode the fixture");
    const UINT stride = static_cast<UINT>(size * 4);
    std::vector<BYTE> pixels(static_cast<size_t>(size) * stride);
    Check(SUCCEEDED(converter->CopyPixels(nullptr, stride,
            static_cast<UINT>(pixels.size()), pixels.data())),
        "original notification filename pipeline must produce pixels");
    return pixels;
}

inline void WriteAlphaPng(const std::filesystem::path& path)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(3, 2)),
        "non-square alpha PNG fixture must initialize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    BYTE pixels[]{ 0, 0, 255, 255, 0, 255, 0, 128, 255, 0, 0, 0,
        255, 255, 255, 64, 0, 0, 0, 255, 30, 60, 90, 200 };
    Check(SUCCEEDED(frame->SetPixelFormat(&format)) &&
            IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(frame->WritePixels(2, 12, sizeof(pixels), pixels)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()),
        "non-square alpha PNG fixture must be written");
}

inline FILE_BASIC_INFO Basic(const std::filesystem::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FILE_BASIC_INFO basic{};
    const bool read = file != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Check(read, "notification fixture file times must be readable");
    return basic;
}

inline void SetBasic(const std::filesystem::path& path, FILE_BASIC_INFO basic)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool written = file != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(file, FileBasicInfo, &basic, sizeof(basic));
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Check(written, "notification fixture file times must be restorable");
}

inline void Run(const std::filesystem::path& root, bool legacyEviction = false)
{
    using snowdesktop::notification_detail::Image;
    Directory directory{ root / L"notification-images" };
    std::filesystem::create_directories(directory.path);
    const auto first = directory.path / L"first.bmp";
    const auto replacement = directory.path / L"replacement.bmp";
    const auto png = directory.path / L"alpha.png";
    const auto large = directory.path / L"large.bmp";
    WriteBitmap(first, 0xff336699u);

    Image image;
    const auto update = [&](const std::wstring& path, int size) {
        // Negative control restores the original Update's unconditional eviction
        // at the production image boundary; WIC and all pixel assertions remain real.
        if (legacyEviction) image.Reset();
        image.Update(path, size);
    };
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff336699u);
    const HBITMAP original = image.Get();
    for (int i = 0; i < 32; ++i)
    {
        update(first.wstring(), 48);
        Check(image.Get() == original,
            "text/progress-only notification updates must retain the decoded bitmap");
        Solid(image.Get(), 48, 0xff336699u);
    }

    const auto before = Basic(first);
    WriteBitmap(first, 0xff112233u);
    auto changed = before;
    changed.ChangeTime.QuadPart += 10000000;
    SetBasic(first, changed);
    const auto after = Basic(first);
    Check(after.LastWriteTime.QuadPart == before.LastWriteTime.QuadPart &&
            after.ChangeTime.QuadPart != before.ChangeTime.QuadPart,
        "same-size overwrite fixture must preserve write time but change change-time");
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff112233u);

    WriteBitmap(first, 0xff224466u);
    SetBasic(first, after);
    const auto restored = Basic(first);
    Check(restored.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart &&
            restored.ChangeTime.QuadPart == after.ChangeTime.QuadPart,
        "in-place rewrite fixture must restore both cached timestamps");
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff224466u);

    // Same path, byte count and timestamps, but a different file identity.
    WriteBitmap(replacement, 0xff557799u);
    SetBasic(replacement, after);
    Check(MoveFileExW(replacement.c_str(), first.c_str(), MOVEFILE_REPLACE_EXISTING),
        "notification image must not keep a file handle that blocks replacement");
    SetBasic(first, after);
    const auto replaced = Basic(first);
    Check(replaced.LastWriteTime.QuadPart == after.LastWriteTime.QuadPart &&
            replaced.ChangeTime.QuadPart == after.ChangeTime.QuadPart,
        "replacement fixture must preserve both cached timestamps");
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff557799u);

    for (const int size : { 72, 96, 48 })
    {
        update(first.wstring(), size);
        Solid(image.Get(), size, 0xff557799u);
    }
    WriteAlphaPng(png);
    for (const int size : { 48, 72, 96 })
    {
        update(png.wstring(), size);
        Check(Pixels(image.Get(), size) == FilenamePixels(png.wstring(), size),
            "notification PNG alpha and Fant scaling must match the original filename decoder");
    }

    WriteBitmap(large, 0xff336699u);
    {
        std::fstream stream(large, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(8 * 1024 * 1024);
        stream.put('\0');
        Check(stream.good(), "large image fixture must exceed the optional snapshot bound");
    }
    update(large.wstring(), 48);
    Solid(image.Get(), 48, 0xff336699u);
    update(large.wstring(), 48);
    Check(Pixels(image.Get(), 48) == FilenamePixels(large.wstring(), 48),
        "the reuse snapshot bound must not reject or change large decodable images");
    update(png.wstring(), 96);

    // An inaccessible source must clear the old image, then retry after recovery.
    HANDLE blocked = CreateFileW(png.c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(blocked != INVALID_HANDLE_VALUE, "notification fixture must be exclusively lockable");
    update(png.wstring(), 96);
    const bool cleared = !image.Get();
    CloseHandle(blocked);
    Check(cleared, "read failure must not keep a stale notification image");
    update(png.wstring(), 96);
    Check(Pixels(image.Get(), 96) == FilenamePixels(png.wstring(), 96),
        "notification decoding must retry after read access recovers");

    std::filesystem::remove(first);
    update(first.wstring(), 48);
    Check(!image.Get(), "a missing notification image must clear previous pixels");
    { std::ofstream stream(first, std::ios::binary); stream << "broken"; }
    update(first.wstring(), 48);
    update(first.wstring(), 48);
    Check(!image.Get(), "malformed notification images must remain empty on repeated updates");
    WriteBitmap(first, 0xff123456u);
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff123456u);
    update(L"", 48);
    Check(!image.Get(), "removing the requested image must release the toast's bitmap");
    update(first.wstring(), 48);
    image.Reset();
    Check(!image.Get(), "toast teardown must discard image and reuse identity together");
    update(first.wstring(), 48);
    Solid(image.Get(), 48, 0xff123456u);
    HBITMAP otherBitmap = nullptr;
    {
        Image otherToast;
        otherToast.Update(png.wstring(), 48);
        otherBitmap = otherToast.Get();
        Check(Pixels(otherToast.Get(), 48) == FilenamePixels(png.wstring(), 48),
            "a new toast must load its own image without cross-notification results");
    }
    BITMAP released{};
    Check(GetObjectW(otherBitmap, sizeof(released), &released) == 0,
        "destroying a toast must delete its GDI bitmap");
    Solid(image.Get(), 48, 0xff123456u);
    std::cout << "notification image reuse, identity, pixels and recovery checks passed\n";
}
}
