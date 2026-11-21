#pragma once

#include <cstddef>
#include <cstdint>

namespace snowdesktop::widget
{
inline constexpr std::uint64_t kMaxArchiveBytes = 20ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxExtractedBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxEntryLuaBytes = 1ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxPreviewBytes = 2ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxResourceBytes = 8ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxPackageFiles = 512;
inline constexpr std::size_t kMaxPackageResources = 32;
inline constexpr std::size_t kMaxRestorablePackageVersions = 2;
}
