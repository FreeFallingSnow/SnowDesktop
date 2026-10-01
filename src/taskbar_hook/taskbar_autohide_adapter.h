#pragma once

#include <windows.h>
#include <array>
#include <cstring>
#include <optional>
#include <span>

namespace snowdesktop::taskbar_hook
{
// Private host/Explorer contract. No process pointers or installed-build RVAs.
struct AutoHideImageIdentity
{
    DWORD timestamp = 0, imageSize = 0;
    GUID pdb{};
    DWORD age = 0;
    friend bool operator==(const AutoHideImageIdentity& a, const AutoHideImageIdentity& b) noexcept
    {
        return a.timestamp == b.timestamp && a.imageSize == b.imageSize &&
            a.age == b.age && std::memcmp(&a.pdb, &b.pdb, sizeof(GUID)) == 0;
    }
};
enum class AutoHideSymbol : std::size_t
{
    PrimaryUnhide, SecondaryUnhide, Focus, FocusCommand, Hotkey, FocusMessage,
    PrimaryWndProc, SecondaryWndProc, Count
};
struct AutoHideFunction
{
    DWORD begin = 0, end = 0;
    std::array<BYTE, 16> entry{};
};
struct AutoHideAdapter
{
    AutoHideImageIdentity image;
    std::array<AutoHideFunction, static_cast<std::size_t>(AutoHideSymbol::Count)> functions{};
    const AutoHideFunction& Get(AutoHideSymbol symbol) const noexcept
    {
        return functions[static_cast<std::size_t>(symbol)];
    }
    bool IsActivationCaller(bool secondary, DWORD caller) const noexcept
    {
        const auto& function = Get(secondary ? AutoHideSymbol::SecondaryWndProc : AutoHideSymbol::PrimaryWndProc);
        return caller > function.begin && caller < function.end;
    }
};

// A SEC_IMAGE mapping or the loaded Explorer image, never a raw file layout.
class AutoHideImageView
{
public:
    explicit AutoHideImageView(std::span<const BYTE> bytes) noexcept : bytes_(bytes) {}
    template<class T> bool Read(std::size_t offset, T& value) const noexcept
    {
        if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset) return false;
        std::memcpy(&value, bytes_.data() + offset, sizeof(T));
        return true;
    }
    std::optional<IMAGE_NT_HEADERS64> Headers() const noexcept
    {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            dos.e_lfanew > 4096 || !Read(static_cast<std::size_t>(dos.e_lfanew), nt) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG ||
            nt.OptionalHeader.SizeOfImage > bytes_.size() ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)) return {};
        return nt;
    }
    std::optional<AutoHideImageIdentity> Identity() const noexcept
    {
        const auto nt = Headers();
        if (!nt) return {};
        const auto debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        if (debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY) || debug.VirtualAddress > bytes_.size() ||
            debug.Size > bytes_.size() - debug.VirtualAddress) return {};
        for (std::size_t offset = 0; offset + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug.Size;
            offset += sizeof(IMAGE_DEBUG_DIRECTORY))
        {
            IMAGE_DEBUG_DIRECTORY entry{};
            if (!Read(debug.VirtualAddress + offset, entry)) return {};
            struct CodeView { DWORD signature; GUID guid; DWORD age; } cv{};
            if (entry.Type == IMAGE_DEBUG_TYPE_CODEVIEW && entry.SizeOfData >= sizeof(cv) &&
                Read(entry.AddressOfRawData, cv) && cv.signature == 0x53445352 && cv.age)
                return AutoHideImageIdentity{nt->FileHeader.TimeDateStamp,
                    nt->OptionalHeader.SizeOfImage, cv.guid, cv.age};
        }
        return {};
    }
    bool Executable(DWORD begin, DWORD end) const noexcept
    {
        const auto nt = Headers();
        IMAGE_DOS_HEADER dos{};
        if (!nt || !Read(0, dos) || !begin || end <= begin || end > nt->OptionalHeader.SizeOfImage) return false;
        const auto sections = static_cast<std::size_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
        for (std::size_t i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            IMAGE_SECTION_HEADER section{};
            if (!Read(sections + i * sizeof(section), section)) return false;
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && begin >= section.VirtualAddress &&
                end - section.VirtualAddress <= section.Misc.VirtualSize) return true;
        }
        return false;
    }
    std::optional<AutoHideFunction> Function(DWORD begin) const noexcept
    {
        const auto nt = Headers();
        if (!nt) return {};
        const auto table = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (table.VirtualAddress > bytes_.size() || table.Size > bytes_.size() - table.VirtualAddress ||
            table.Size % sizeof(RUNTIME_FUNCTION)) return {};
        for (std::size_t i = 0; i < table.Size; i += sizeof(RUNTIME_FUNCTION))
        {
            RUNTIME_FUNCTION runtime{};
            if (!Read(table.VirtualAddress + i, runtime)) return {};
            if (runtime.BeginAddress != begin) continue;
            AutoHideFunction result{begin, runtime.EndAddress};
            if (runtime.EndAddress - begin < result.entry.size() ||
                !Executable(begin, runtime.EndAddress) || !Read(begin, result.entry)) return {};
            return result;
        }
        return {};
    }
    bool Validate(const AutoHideAdapter& adapter) const noexcept
    {
        const auto identity = Identity();
        if (!identity || *identity != adapter.image) return false;
        for (std::size_t i = 0; i < adapter.functions.size(); ++i)
        {
            const auto& expected = adapter.functions[i];
            const auto actual = Function(expected.begin);
            if (!actual || actual->end != expected.end || actual->entry != expected.entry) return false;
            for (std::size_t j = 0; j < i; ++j)
                if (adapter.functions[j].begin == expected.begin) return false;
        }
        return true;
    }
private:
    std::span<const BYTE> bytes_;
};
}
