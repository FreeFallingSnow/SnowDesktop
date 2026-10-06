#pragma once

#include "taskbar_autohide_adapter.h"
#include "tray_modern_adapter.h"
#include <filesystem>
#include <optional>

namespace snowdesktop::taskbar_hook
{
inline constexpr wchar_t kSymbolHelperCommand[] = L"--internal-taskbar-symbols";
struct AutoHideResolution
{
    DWORD magic = 0x53445359, version = 2;
    DWORD error = ERROR_IO_PENDING;
    AutoHideAdapter adapter;
    DWORD modernTrayError = ERROR_IO_PENDING;
    ModernTrayAdapter modernTray;
};
// These functions run in the host/helper, never inside Explorer. DbgHelp has
// process-global, single-threaded state; the isolated helper owns all its calls.
std::optional<int> TryRunTaskbarSymbolHelper();
AutoHideResolution ResolveTaskbarSymbols(const std::filesystem::path& cacheRoot);
AutoHideResolution RunTaskbarSymbolHelper(const std::filesystem::path& executable,
    const std::filesystem::path& cacheRoot, HANDLE cancel, DWORD timeoutMs = 60000);
}
