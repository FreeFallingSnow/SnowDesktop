#pragma once

#include "hook_entry_protocol.h"
#include <filesystem>

namespace snowdesktop::hook_entry
{
// Publish an immutable implementation image through the current host catalog,
// then return the stable image whose exports are supplied to SetWindowsHookEx.
// An already mapped, compatible entry remains usable across host updates.
// Returned entry and published implementation paths identify the physical
// files, including when MSIX redirects newly created AppData files.
std::wstring Prepare(const std::filesystem::path& cacheRoot,
    const std::filesystem::path& entrySource, Payload payload,
    const std::filesystem::path& implementation);
bool HasEntryAbi(const std::filesystem::path& image, WORD expectedMachine);
std::filesystem::path CurrentImplementation(Payload payload);
}
