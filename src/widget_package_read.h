#pragma once

#include <filesystem>
#include <string>

namespace snowdesktop::widget::detail
{
// Internal IO policy. Settings review may read with compatible sharing when
// its optional identity lock is unavailable. Other fingerprint callers retain
// their original read-only sharing through WidgetPackageManager::Sha256File.
std::string HashPackageFile(const std::filesystem::path& path,
    bool allowConcurrentAccess);
}
