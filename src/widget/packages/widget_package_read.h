#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace snowdesktop::widget::detail
{
std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::string PathUtf8(const std::filesystem::path& path);
bool ReadPackageFile(const std::filesystem::path& path, std::string& output,
    std::uint64_t maxBytes);
bool StartsWithPath(const std::filesystem::path& child,
    const std::filesystem::path& root);
bool HasReparsePoint(const std::filesystem::path& path);

std::string Timestamp();

// Internal IO policy. Settings review may read with compatible sharing when
// its optional identity lock is unavailable. Other fingerprint callers retain
// their original read-only sharing through WidgetPackageManager::Sha256File.
std::string HashPackageFile(const std::filesystem::path& path,
    bool allowConcurrentAccess);
}
