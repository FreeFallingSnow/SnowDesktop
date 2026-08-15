#include "widget_package_image_cache.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <windows.h>

namespace
{
namespace fs = std::filesystem;
using snowdesktop::widget_runtime::WidgetPackageImageCache;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void WriteBitmap(const fs::path& path, std::uint32_t color)
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
    Check(stream.good(), "test bitmap must be written");
}
}

int main()
{
    const HRESULT initialized = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE,
        "COM must initialize for WIC decoding");

    const fs::path root = fs::temp_directory_path() /
        (L"SnowDesktopWidgetPackageImageCacheTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    error.clear();
    fs::create_directories(root, error);
    Check(!error, "test directory must be created");

    const fs::path first = root / L"first.bmp";
    const fs::path second = root / L"second.bmp";
    const fs::path invalid = root / L"invalid.bmp";
    WriteBitmap(first, 0xff336699u);
    WriteBitmap(second, 0xff112233u);
    {
        std::ofstream stream(invalid, std::ios::binary | std::ios::trunc);
        stream << "not-an-image";
    }

    WidgetPackageImageCache cache;
    const auto* decoded = cache.Load(first.wstring());
    Check(decoded && decoded->width == 1 && decoded->height == 1 &&
            decoded->stride == 4 && decoded->pixels.size() == 4 &&
            cache.Size() == 1 && cache.Bytes() == 4,
        "valid package image must decode into bounded BGRA pixels");
    const auto* reused = cache.Load(first.wstring());
    Check(reused == decoded, "the same canonical path must share one source");

    fs::remove(first, error);
    Check(!error && cache.Find(first.wstring()) == decoded &&
            cache.Load(first.wstring()) == decoded,
        "render-side lookup must survive source removal without file access");
    Check(!cache.Load(invalid.wstring()) && cache.Failed(invalid.wstring()),
        "decode failures must be stable and negatively cached");

    WidgetPackageImageCache quotaCache(4, 4);
    WriteBitmap(first, 0xff336699u);
    Check(quotaCache.Load(first.wstring()) &&
            !quotaCache.Load(second.wstring()) &&
            quotaCache.Failed(second.wstring()) && quotaCache.Bytes() == 4,
        "decoded image cache must enforce per-image and total byte quotas");
    quotaCache.Clear();
    Check(quotaCache.Size() == 0 && quotaCache.Bytes() == 0 &&
            !quotaCache.Failed(second.wstring()),
        "cache clear must release pixels and negative entries");

    fs::remove_all(root, error);
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::cout << "widget package image cache tests passed\n";
    return 0;
}
