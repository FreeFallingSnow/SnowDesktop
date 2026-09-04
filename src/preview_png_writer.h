#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace snowdesktop::preview_png
{

/** Atomically write premultiplied, top-down BGRA pixels to a PNG file. */
bool Save(const std::filesystem::path& output,
    int width, int height, std::span<const std::uint32_t> pixels,
    std::string& error);

} // namespace snowdesktop::preview_png
