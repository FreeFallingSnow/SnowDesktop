#pragma once

#include <windows.h>
#include <cstring>
#include <stdexcept>
#include <string>

namespace snowdesktop::test
{
// Redirect only an import in this owned regression executable. Never patch
// another process, the desktop host, Explorer, or a system DLL's code.
class NativeImportPatch
{
public:
    ULONG_PTR original = 0;
    NativeImportPatch(const char* name, ULONG_PTR replacement)
    {
        auto* module = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module +
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        for (auto* entry = imports; entry->Name && !slot_; ++entry)
        {
            if (!entry->OriginalFirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(module + entry->OriginalFirstThunk);
            auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(module + entry->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++addresses)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(module + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), name) == 0)
                { slot_ = &addresses->u1.Function; break; }
            }
        }
        if (!slot_) throw std::runtime_error(std::string("missing native import: ") + name);
        DWORD protection = 0;
        if (!VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection))
            throw std::runtime_error("cannot redirect owned native import");
        original = *slot_;
        *slot_ = replacement;
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
    }
    ~NativeImportPatch()
    {
        DWORD protection = 0;
        if (slot_ && VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection))
        {
            *slot_ = original;
            DWORD ignored = 0;
            VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
        }
    }
    NativeImportPatch(const NativeImportPatch&) = delete;
    NativeImportPatch& operator=(const NativeImportPatch&) = delete;
private:
    ULONG_PTR* slot_ = nullptr;
};
}
