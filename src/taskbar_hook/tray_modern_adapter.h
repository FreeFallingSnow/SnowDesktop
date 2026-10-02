#pragma once
#include "taskbar_autohide_adapter.h"
#include <initializer_list>

namespace snowdesktop::taskbar_hook
{
// Optional, read-only Explorer supplement. All addresses come from the PDB
// matching the installed image; layouts are accepted only when the named
// methods still demonstrate the expected ABI. No Windows-build RVA table.
enum class ModernTraySymbol : std::size_t
{ CopyData, Notify, GetRect, Identity, SendOwner, SetIcon, SetTooltip, StringBuffer, Promoted, Overflow, VectorSize, VectorAt, Count };
struct ModernTrayLayout
{
    DWORD model = 0, manager = 0;
    DWORD window = 0, id = 0, guid = 0, callback = 0, version = 0, icon = 0, tip = 0;
    DWORD promoted = 0, overflow = 0, vectorAdjustment = 0, vectorBegin = 0, vectorEnd = 0;
    friend bool operator==(const ModernTrayLayout&, const ModernTrayLayout&) = default;
};
struct ModernTrayAdapter
{
    AutoHideImageIdentity image;
    DWORD root = 0, itemVtable = 0;
    std::array<AutoHideFunction, static_cast<std::size_t>(ModernTraySymbol::Count)> functions{};
    ModernTrayLayout layout;
    const AutoHideFunction& Get(ModernTraySymbol symbol) const { return functions[static_cast<std::size_t>(symbol)]; }
};
inline std::optional<std::size_t> FindTrayInstruction(std::span<const BYTE> code, std::initializer_list<int> pattern)
{
    std::optional<std::size_t> found;
    if (code.size() < pattern.size()) return {};
    for (std::size_t i = 0; i <= code.size() - pattern.size(); ++i)
    {
        bool match = true; std::size_t j = 0;
        for (int byte : pattern) { if (byte >= 0 && code[i + j] != byte) match = false; ++j; }
        if (match) { if (found) return {}; found = i; }
    }
    return found;
}
inline std::optional<ModernTrayLayout> DecodeModernTrayLayout(std::span<const BYTE> image, const ModernTrayAdapter& adapter)
{
    const AutoHideImageView view(image);
    if (view.Identity() != std::optional(adapter.image) || !adapter.root || !adapter.itemVtable ||
        adapter.root > image.size() || 8 > image.size() - adapter.root ||
        adapter.itemVtable > image.size() || 8 > image.size() - adapter.itemVtable ||
        view.Executable(adapter.root, adapter.root + 8) || view.Executable(adapter.itemVtable, adapter.itemVtable + 8)) return {};
    for (std::size_t i = 0; i < adapter.functions.size(); ++i)
    {
        const auto& function = adapter.functions[i];
        if (i == static_cast<std::size_t>(ModernTraySymbol::StringBuffer))
        {
            std::array<BYTE, 16> entry{};
            if (!view.Executable(function.begin, function.end) || function.end - function.begin != 21 ||
                !view.Read(function.begin, entry) || entry != function.entry) return {};
            continue;
        }
        const auto actual = view.Function(function.begin);
        if (!actual || actual->end != function.end || actual->entry != function.entry) return {};
    }
    const auto code = [&](ModernTraySymbol symbol) {
        const auto& f = adapter.Get(symbol); return image.subspan(f.begin, f.end - f.begin);
    };
    const auto displacement = [&](ModernTraySymbol symbol, std::initializer_list<int> pattern,
        std::size_t at, bool byte = false) -> std::optional<DWORD> {
        const auto bytes = code(symbol); const auto match = FindTrayInstruction(bytes, pattern);
        DWORD value = 0;
        if (!match || *match + at > bytes.size() || (byte ? 1u : 4u) > bytes.size() - *match - at) return {};
        if (byte) value = bytes[*match + at]; else std::memcpy(&value, bytes.data() + *match + at, 4);
        if (!value || value > 0x1000) return {}; return value;
    };
    using S = ModernTraySymbol;
    ModernTrayLayout result;
    // CopyData obtains the taskbar model, then its shared notification manager.
    const auto copy = code(S::CopyData);
    const auto call = FindTrayInstruction(copy, {0x48,0x8d,0x4d,0xe0,0xe8,-1,-1,-1,-1,0x90,
        0x49,0x8b,0xd0,0x48,0x8b,0x4d,0xe0,0xe8,-1,-1,-1,-1,0x0f,0xb6,0xc0});
    if (!call || *call < 19) return {};
    const auto start = *call - 19;
    if (!FindTrayInstruction(copy.subspan(start, 19), {0x48,0x8b,0x97,-1,-1,-1,-1,
        0x48,0x85,0xd2,0x74,-1,0x48,0x81,0xc2,-1,-1,-1,-1})) return {};
    std::int32_t relative = 0; std::memcpy(&relative, copy.data() + *call + 18, 4);
    if (static_cast<std::int64_t>(adapter.Get(S::CopyData).begin) + static_cast<std::int64_t>(*call) + 22 + relative != adapter.Get(S::Notify).begin) return {};
    std::memcpy(&result.model, copy.data() + start + 3, 4);
    std::memcpy(&result.manager, copy.data() + start + 15, 4);
    // The manager owns a bounded vector of implementation pointers, not a map.
    if (!FindTrayInstruction(code(S::GetRect), {0x4c,0x8b,0x43,0x08,0x48,0x8b,0x13}) ||
        !FindTrayInstruction(code(S::Identity), {0x48,0x89,0x02})) return {};
    const auto window = displacement(S::Identity, {0x48,0x8b,0x41,-1,0x48,0x8b,0xda,0x48,0x89,0x02}, 3, true);
    const auto id = displacement(S::Identity, {0x8b,0x41,-1,0x89,0x42,0x08}, 2, true);
    const auto guid = displacement(S::Identity, {0x0f,0x10,0x41,-1,0xf3,0x0f,0x7f,0x42,0x0c}, 3, true);
    const auto callback = displacement(S::SendOwner, {0x8b,0x93,-1,-1,-1,-1,0x85,0xd2}, 2);
    const auto version = displacement(S::SendOwner, {0x83,0xbb,-1,-1,-1,-1,0x04}, 2);
    const auto icon = displacement(S::SetIcon, {0x48,0x8d,0x9e,-1,-1,-1,-1,0x48,0x39,0x3b,0x0f,0x84}, 3);
    const auto tip = displacement(S::SetTooltip, {0x49,0x8d,0x8e,-1,-1,-1,-1,0x48,0x8d,0x44,0x24,0x20}, 3);
    const auto promoted = displacement(S::Promoted, {0x48,0x8b,0x41,-1,0x48,0x8b,0xda,0x48,0x8b,0xca,0x48,0x89,0x02}, 3, true);
    const auto overflow = displacement(S::Overflow, {0x48,0x8b,0x41,-1,0x48,0x8b,0xda,0x48,0x8b,0xca,0x48,0x89,0x02}, 3, true);
    const auto adjustment = displacement(S::VectorSize, {0x48,0x8d,0x41,-1,0x48,0xf7,0xd9}, 3, true);
    const auto begin = displacement(S::VectorSize, {0x48,0x2b,0x50,-1,0x48,0xc1,0xfa,0x03}, 3, true);
    const auto end = displacement(S::VectorSize, {0x48,0x8b,0x51,-1,0x49,0xf7,0xd8}, 3, true);
    if (!window || !id || !guid || !callback || !version || !icon || !tip || !promoted || !overflow ||
        !adjustment || !begin || !end || *end != *begin + 8 || *adjustment < 0x80 ||
        !FindTrayInstruction(code(S::StringBuffer), {0x48,0x8b,0x01,0x48,0x85,0xc0,0x74,0x05,0x48,0x8b,0x40,0x10,0xc3})) return {};
    result.window = *window; result.id = *id; result.guid = *guid; result.callback = *callback;
    result.version = *version; result.icon = *icon; result.tip = *tip;
    result.promoted = *promoted; result.overflow = *overflow;
    result.vectorAdjustment = 256 - *adjustment; result.vectorBegin = *begin; result.vectorEnd = *end;
    if (result.model > 0x1000 || result.manager > 0x1000 || !result.model || !result.manager ||
        result.id != result.window + 8 || result.guid != result.id + 4 ||
        result.callback != result.version + 4 || result.vectorAdjustment > result.vectorBegin) return {};
    if (!FindTrayInstruction(code(S::SendOwner), {0x48,0x8b,0x4b,static_cast<int>(result.window),0xff,0x15}) ||
        !FindTrayInstruction(code(S::SendOwner), {0x44,0x8b,0x43,static_cast<int>(result.id),0x4c,0x8b,0xce}) ||
        !FindTrayInstruction(code(S::VectorAt), {0x48,0x8d,0x42,static_cast<int>(*adjustment),0x48,0xf7,0xda})) return {};
    return result;
}
}
