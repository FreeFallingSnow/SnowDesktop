#pragma once

#include <windows.h>

#include <string>

namespace snowdesktop::item_location
{

// Returns the path Explorer should reveal. A .lnk resolves to its target when
// that target has a file-system path; other paths are returned unchanged.
std::wstring ResolveRevealPath(const std::wstring& path);

// True when ResolveRevealPath produces an existing file-system item.
bool CanReveal(const std::wstring& path);

// Opens Explorer with the resolved item selected.
bool Reveal(HWND owner, const std::wstring& path);

} // namespace snowdesktop::item_location
