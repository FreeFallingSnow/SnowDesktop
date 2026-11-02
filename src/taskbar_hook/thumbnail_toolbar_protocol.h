#pragma once
#include "taskbar_autohide_adapter.h"
#include <shobjidl.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace snowdesktop::thumbnail_toolbar
{
inline constexpr wchar_t kMessage[] = L"SnowDesktop.ThumbnailToolbar.Query.v1";
inline constexpr DWORD kMagic = 0x53445448, kVersion = 1;
inline constexpr size_t kMaximumButtons = 7, kIconExtent = 64;
enum class Symbol : size_t { FindWindow, GetButtons, GetImageList, GetWindow, Count };
struct Adapter
{
    taskbar_hook::AutoHideImageIdentity image;
    DWORD windowVtable = 0, managerVtable = 0;
    std::array<taskbar_hook::AutoHideFunction, static_cast<size_t>(Symbol::Count)> functions{};
    const auto& Get(Symbol symbol) const { return functions[static_cast<size_t>(symbol)]; }
};
inline bool Validate(std::span<const BYTE> bytes, const Adapter& adapter)
{
    const taskbar_hook::AutoHideImageView image(bytes);
    if (image.Identity() != std::optional(adapter.image)) return false;
    for (DWORD table : {adapter.windowVtable, adapter.managerVtable})
        if (!table || table > bytes.size() || 8 > bytes.size() - table || image.Executable(table, table + 8)) return false;
    for (const auto& function : adapter.functions)
    {
        std::array<BYTE, 16> entry{};
        if (!image.Executable(function.begin, function.end) ||
            function.end - function.begin < entry.size() ||
            !image.Read(function.begin, entry) || entry != function.entry) return false;
    }
    return true;
}
#pragma pack(push, 4)
// Explorer's cross-bitness record, also used by the 64-bit Shell.
struct Button32
{
    DWORD mask = 0, id = 0, bitmap = 0, icon = 0;
    wchar_t tip[260]{};
    DWORD flags = 0;
    bool operator==(const Button32&) const = default;
};
#pragma pack(pop)
static_assert(sizeof(Button32) == 540);
struct Button
{
    Button32 native;
    DWORD width = 0, height = 0;
    std::array<std::uint32_t, kIconExtent * kIconExtent> pixels{};
    bool operator==(const Button&) const = default;
};
struct Target
{
    std::uint64_t window = 0, processCreation = 0;
    DWORD process = 0, thread = 0;
    bool operator==(const Target&) const = default;
};
struct Snapshot
{
    Target target;
    std::uint64_t identity = 0;
    DWORD count = 0;
    std::array<Button, kMaximumButtons> buttons{};
    bool operator==(const Snapshot&) const = default;
};
inline bool Visible(const Button32& button) { return !(button.flags & THBF_HIDDEN); }
inline bool Interactive(const Button32& button)
{ return Visible(button) && !(button.flags & (THBF_DISABLED | THBF_NONINTERACTIVE)); }
inline size_t VisibleCount(const Snapshot& snapshot)
{
    if (snapshot.count > kMaximumButtons) return 0;
    return static_cast<size_t>(std::count_if(snapshot.buttons.begin(), snapshot.buttons.begin() + snapshot.count,
        [](const Button& button) { return Visible(button.native); }));
}
inline std::wstring MappingName(DWORD owner, std::uint64_t nonce)
{ return L"Local\\SnowDesktop.ThumbnailToolbar.v1." + std::to_wstring(owner) + L"." + std::to_wstring(nonce); }
struct Request
{
    DWORD magic = kMagic, version = kVersion, bytes = sizeof(Request), owner = 0;
    std::uint64_t ownerCreation = 0;
    Adapter adapter;
    Target target;
    DWORD click = FALSE, button = 0;
    std::uint64_t expectedIdentity = 0;
    volatile LONG complete = 0;
    DWORD error = ERROR_IO_PENDING, dismiss = FALSE;
    Snapshot snapshot;
};
// Called only through the stable registry-query Hook entry, on the Taskband thread.
void Process(HWND taskband, DWORD owner, std::uint64_t nonce) noexcept;
}
