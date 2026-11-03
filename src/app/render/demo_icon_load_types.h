#pragma once

#include "icons/icon_beautify.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

struct DemoIconDecodeResult {
    std::uint64_t generation = 0;
    std::size_t visualIndex = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> pixels;
    bool needsGlassBackdrop = false;
};

struct DemoIconLoadTask {
    std::uint64_t generation = 0;
    std::size_t visualIndex = 0;
    std::filesystem::path path;
    snowdesktop::IconBeautifySettings beautify;
};
