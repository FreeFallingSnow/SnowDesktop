#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::icon_image
{
inline constexpr std::uint64_t kMaxFileBytes = 16ull * 1024 * 1024;
inline constexpr std::uint64_t kMaxPixels = 16000000;
inline constexpr unsigned kCacheVersion = 2;

struct Image
{
    int width = 0, height = 0;
    int sourceWidth = 0, sourceHeight = 0;
    bool vector = false;
    std::string format;
    // Always top-down, premultiplied BGRA. This is NOT a Shell bitmap.
    std::vector<std::uint32_t> pixels;
};

// edge == 0 retains raster dimensions (SVG defaults to 1024 pixels).
bool Decode(const std::filesystem::path& path, int edge, Image& image, std::string& error);
bool DecodeBytes(std::string_view bytes, int edge, Image& image, std::string& error);
bool EncodePng(const Image& image, std::string& bytes, std::string& error);
bool PadSquare(Image& image);
HBITMAP CreateBitmap(const Image& image);
bool ExportPng(const std::filesystem::path& source, const std::filesystem::path& output,
    std::string& error);
bool ExportIco(const std::filesystem::path& source, const std::filesystem::path& output,
    std::string& error);
}
