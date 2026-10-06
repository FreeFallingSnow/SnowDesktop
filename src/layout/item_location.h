#pragma once

#include <windows.h>

#include <string>

namespace snowdesktop::item_location
{

enum class FolderTargetKind
{
    None,
    Directory,
    Shortcut,
};

struct FolderTarget
{
    std::wstring path;
    FolderTargetKind kind = FolderTargetKind::None;
    bool available = false;

    explicit operator bool() const noexcept
    {
        return available && !path.empty();
    }
};

// Returns the path Explorer should reveal. A .lnk resolves to its target when
// that target has a file-system path; other paths are returned unchanged.
std::wstring ResolveRevealPath(const std::wstring& path);

// Resolves a real file-system directory or a .lnk whose target is one.
// Unavailable shortcut targets retain their resolved path and kind with
// available=false. A direct unavailable path or existing non-directory target
// returns an empty result because its original type cannot be proven safely.
FolderTarget ResolveFolderTarget(const std::wstring& path);

// True when ResolveRevealPath produces an existing file-system item.
bool CanReveal(const std::wstring& path);

// Opens Explorer with the resolved item selected.
bool Reveal(HWND owner, const std::wstring& path);

} // namespace snowdesktop::item_location
