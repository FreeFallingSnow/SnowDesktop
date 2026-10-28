#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace snowdesktop::video_writer
{
// Integer frame boundaries avoid accumulated rounding drift at rates such as
// 30fps. Time is in Media Foundation's 100ns units.
std::int64_t FrameTime(std::uint64_t index, unsigned fps);
void ToNv12(std::span<const std::uint32_t> bgra, unsigned width, unsigned height,
    std::span<std::uint8_t> nv12);

// The caller owns the COM apartment and supplies an exclusively owned output
// path. MF DLLs are loaded lazily; Windows editions without media components
// still start the desktop normally and report an export error on use.
class Mp4 final
{
public:
    Mp4(const std::filesystem::path& path, unsigned width, unsigned height,
        unsigned fps, unsigned bitrate = 12000000);
    ~Mp4();
    Mp4(const Mp4&) = delete;
    Mp4& operator=(const Mp4&) = delete;
    void Write(std::span<const std::uint32_t> bgra);
    void Finish();
    std::uint64_t FrameCount() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
