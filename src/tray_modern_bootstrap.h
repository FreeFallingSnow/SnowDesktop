#pragma once
#include "taskbar_hook/tray_modern_adapter.h"
#include "tray_icon_pixels.h"
#include <tlhelp32.h>
#include <vector>

namespace snowdesktop::tray
{
class ModernTrayReader
{
public:
    explicit ModernTrayReader(DWORD process) : process_(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, process)) {}
    ~ModernTrayReader() { if (process_) CloseHandle(process_); }
    ModernTrayReader(const ModernTrayReader&) = delete;
    template<class T> bool Read(std::uint64_t address, T& value) const { return Bytes(address, &value, sizeof(value)); }
    bool Bytes(std::uint64_t address, void* output, std::size_t size) const
    {
        SIZE_T copied = 0;
        return process_ && address >= 0x10000 && address < 0x0000800000000000 &&
            size <= 65536 && size < 0x0000800000000000 - address &&
            ReadProcessMemory(process_, reinterpret_cast<const void*>(address), output, size, &copied) && copied == size;
    }
    bool Vector(std::uint64_t address, std::vector<std::uint64_t>& items) const
    {
        std::array<std::uint64_t, 2> bounds{};
        if (!Read(address, bounds) || bounds[1] < bounds[0] || (bounds[1] - bounds[0]) % 8 ||
            bounds[1] - bounds[0] > kGeometries * 8) return false;
        items.resize(static_cast<std::size_t>((bounds[1] - bounds[0]) / 8));
        if (!items.empty() && !Bytes(bounds[0], items.data(), items.size() * 8)) return false;
        std::array<std::uint64_t, 2> after{};
        return Read(address, after) && after == bounds;
    }
    bool Image(std::uint64_t base, const taskbar_hook::ModernTrayAdapter& adapter) const
    {
        using namespace taskbar_hook;
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
        if (!Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
            !Read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.TimeDateStamp != adapter.image.timestamp || nt.OptionalHeader.SizeOfImage != adapter.image.imageSize ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG) return false;
        const auto debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        if (debug.Size > 4096 || debug.VirtualAddress >= adapter.image.imageSize ||
            debug.Size > adapter.image.imageSize - debug.VirtualAddress) return false;
        bool matching = false;
        for (std::size_t offset = 0; offset + sizeof(IMAGE_DEBUG_DIRECTORY) <= debug.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY))
        {
            IMAGE_DEBUG_DIRECTORY entry{};
            if (!Read(base + debug.VirtualAddress + offset, entry)) return false;
            struct CodeView { DWORD signature; GUID guid; DWORD age; } cv{};
            if (entry.Type == IMAGE_DEBUG_TYPE_CODEVIEW && entry.SizeOfData >= sizeof(cv) &&
                entry.AddressOfRawData < adapter.image.imageSize && sizeof(cv) <= adapter.image.imageSize - entry.AddressOfRawData &&
                Read(base + entry.AddressOfRawData, cv) && cv.signature == 0x53445352 &&
                cv.guid == adapter.image.pdb && cv.age == adapter.image.age) matching = true;
        }
        if (!matching) return false;
        for (const auto& function : adapter.functions)
        {
            std::array<BYTE, 16> entry{};
            if (function.begin >= adapter.image.imageSize || entry.size() > adapter.image.imageSize - function.begin ||
                !Read(base + function.begin, entry) || entry != function.entry) return false;
        }
        return true;
    }
    bool Visible(std::uint64_t collection, std::uint64_t base, const taskbar_hook::ModernTrayAdapter& adapter,
        std::vector<std::uint64_t>& visible) const
    {
        // Locate only this object's bounded COM subobjects, by the two methods
        // of the exact typed IVector ABI resolved from the matching PDB.
        const auto& layout = adapter.layout;
        for (int offset = -32; offset <= 64; offset += 8)
        {
            const auto candidate = static_cast<std::uint64_t>(static_cast<std::int64_t>(collection) + offset);
            std::uint64_t vtable = 0, at = 0, size = 0;
            if (!Read(candidate, vtable) || !Read(vtable + 6 * 8, at) || !Read(vtable + 7 * 8, size) ||
                at != base + adapter.Get(taskbar_hook::ModernTraySymbol::VectorAt).begin ||
                size != base + adapter.Get(taskbar_hook::ModernTraySymbol::VectorSize).begin) continue;
            return Vector(candidate - layout.vectorAdjustment + layout.vectorBegin, visible);
        }
        return false;
    }
    void Tooltip(std::uint64_t value, wchar_t (&tip)[128]) const
    {
        std::uint64_t buffer = 0;
        if (!value || !Read(value + 16, buffer)) return;
        for (std::size_t i = 0; i + 1 < std::size(tip); ++i)
            if (!Read(buffer + i * 2, tip[i]) || !tip[i]) break;
        tip[127] = 0;
    }
private:
    HANDLE process_ = nullptr;
};
inline std::uint64_t ModernTaskbarModule(DWORD process)
{
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
    if (modules == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module{}; module.dwSize = sizeof(module); std::uint64_t base = 0;
    if (Module32FirstW(modules, &module)) do {
        if (!_wcsicmp(module.szModule, L"Taskbar.dll")) { base = reinterpret_cast<std::uint64_t>(module.modBaseAddr); break; }
    } while (Module32NextW(modules, &module));
    CloseHandle(modules); return base;
}
inline std::vector<Event> BootstrapModernTray(DWORD explorer, std::uint64_t epoch,
    const taskbar_hook::ModernTrayAdapter& adapter)
{
    std::vector<Event> events;
    const auto base = ModernTaskbarModule(explorer);
    ModernTrayReader reader(explorer);
    if (!base || !reader.Image(base, adapter)) return events;
    const auto& layout = adapter.layout;
    std::uint64_t root = 0, model = 0, manager = 0, promoted = 0, overflow = 0;
    if (!reader.Read(base + adapter.root, root) || !reader.Read(root + layout.model, model) ||
        !reader.Read(model + layout.manager, manager) || !reader.Read(manager + layout.promoted, promoted) ||
        !reader.Read(manager + layout.overflow, overflow)) return events;
    std::vector<std::uint64_t> records, pinned, hidden;
    if (!reader.Vector(manager, records) || !reader.Visible(promoted, base, adapter, pinned) ||
        !reader.Visible(overflow, base, adapter, hidden)) return events;
    for (const auto record : records)
    {
        const auto length = (std::max)({layout.guid + 16, layout.window + 8, layout.id + 4,
            layout.callback + 4, layout.version + 4, layout.icon + 8, layout.tip + 8});
        if (length > 4096) return {};
        std::array<BYTE, 4096> before{}, after{};
        if (!reader.Bytes(record, before.data(), length)) return {};
        const auto get = [&]<class T>(DWORD offset, T& value) { std::memcpy(&value, before.data() + offset, sizeof(value)); };
        std::uint64_t vtable = 0, iconHandle = 0, tip = 0; Event event;
        get(0, vtable);
        if (vtable != base + adapter.itemVtable) return {};
        get(layout.window, event.identity.window); get(layout.id, event.identity.id);
        get(layout.guid, event.identity.guid); get(layout.callback, event.callback);
        get(layout.version, event.version); get(layout.icon, iconHandle); get(layout.tip, tip);
        const auto target = reinterpret_cast<HWND>(event.identity.window);
        const DWORD thread = GetWindowThreadProcessId(target, &event.identity.process);
        if (!thread || !event.identity.process || event.callback < WM_USER || event.callback > 0xffff ||
            (event.version != 0 && event.version != NOTIFYICON_VERSION && event.version != NOTIFYICON_VERSION_4)) continue;
        event.epoch = epoch; event.operation = kBootstrapIcon;
        event.flags = NIF_MESSAGE | NIF_ICON | NIF_STATE;
        if (HasGuid(event.identity.guid)) event.flags |= NIF_GUID;
        // Overflow placement is visible; only absence from BOTH native visible
        // collections means NIS_HIDDEN. Promotion preference is not visibility.
        std::uint64_t projected = 0;
        // Find the record's actual WinRT interface in the native collections.
        bool visible = false;
        for (const auto& list : {&pinned, &hidden}) for (const auto value : *list)
            if (value >= record && value - record <= 64 && reader.Read(value, projected) &&
                projected >= base && projected < base + adapter.image.imageSize) visible = true;
        event.stateMask = NIS_HIDDEN; event.state = visible ? 0 : NIS_HIDDEN;
        reader.Tooltip(tip, event.tip); if (event.tip[0]) event.flags |= NIF_TIP;
        if (auto copy = CopyIcon(reinterpret_cast<HICON>(iconHandle))) { Pixels(copy, event); DestroyIcon(copy); }
        DWORD processAfter = 0;
        if (!reader.Bytes(record, after.data(), length) || !std::equal(before.begin(), before.begin() + length, after.begin()) ||
            GetWindowThreadProcessId(target, &processAfter) != thread || processAfter != event.identity.process) return {};
        if (event.width && event.height) events.push_back(event);
    }
    // Do not deliver a partly stale snapshot after a concurrent Shell mutation.
    std::vector<std::uint64_t> check, checkPinned, checkHidden;
    std::uint64_t checkRoot = 0, checkModel = 0, checkManager = 0, checkPromoted = 0, checkOverflow = 0;
    if (!reader.Read(base + adapter.root, checkRoot) || checkRoot != root ||
        !reader.Read(root + layout.model, checkModel) || checkModel != model ||
        !reader.Read(model + layout.manager, checkManager) || checkManager != manager ||
        !reader.Read(manager + layout.promoted, checkPromoted) || checkPromoted != promoted ||
        !reader.Read(manager + layout.overflow, checkOverflow) || checkOverflow != overflow ||
        !reader.Vector(manager, check) || check != records ||
        !reader.Visible(promoted, base, adapter, checkPinned) || checkPinned != pinned ||
        !reader.Visible(overflow, base, adapter, checkHidden) || checkHidden != hidden) return {};
    return events;
}
}
