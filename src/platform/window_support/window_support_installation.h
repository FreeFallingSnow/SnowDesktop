#pragma once
#include <windows.h>
#include <filesystem>

namespace snowdesktop::window_support
{
// Publish an already complete bundle under the fixed authorized path. Mapped
// images remain at retiredDirectory; this never deletes a live Hook DLL.
DWORD PublishHelperBundle(const std::filesystem::path& staging, const std::filesystem::path& destination,
    std::filesystem::path& retiredDirectory) noexcept;
}
