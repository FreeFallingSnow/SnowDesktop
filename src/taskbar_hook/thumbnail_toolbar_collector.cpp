#include "thumbnail_toolbar_protocol.h"
#include "platform/hook_entry_protocol.h"
#include "system/tray/tray_icon_pixels.h"
#include <commctrl.h>
#include <wrl/client.h>
#include <cstring>
#include <climits>

namespace snowdesktop::thumbnail_toolbar
{
namespace
{
template<class T> bool Read(const void* source, T& result)
{
    SIZE_T copied = 0;
    return source && ReadProcessMemory(GetCurrentProcess(), source, &result, sizeof(result), &copied) && copied == sizeof(result);
}
bool Current(const Target& target)
{
    DWORD process = 0;
    if (!target.window || GetWindowThreadProcessId(reinterpret_cast<HWND>(target.window), &process) != target.thread ||
        process != target.process) return false;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    const auto creation = handle ? hook_entry::ProcessCreation(handle) : 0;
    if (handle) CloseHandle(handle);
    return creation && creation == target.processCreation;
}
bool HasMethod(IUnknown* item, const BYTE* method)
{
    std::uintptr_t table = 0;
    if (!Read(item, table)) return false;
    for (size_t index = 3; index < 80; ++index)
    {
        const BYTE* entry = nullptr;
        if (!Read(reinterpret_cast<const void*>(table + index * sizeof(void*)), entry)) return false;
        if (entry == method) return true;
    }
    return false;
}
void Collect(HWND taskband, Request& request)
{
    request.error = ERROR_NOT_SUPPORTED;
    const HMODULE module = GetModuleHandleW(L"Taskbar.dll");
    if (!module || request.adapter.image.imageSize > 64 * 1024 * 1024 || !Validate({reinterpret_cast<const BYTE*>(module), request.adapter.image.imageSize}, request.adapter)) return;
    DWORD taskbandProcess = 0;
    if (GetWindowThreadProcessId(taskband, &taskbandProcess) != GetCurrentThreadId() ||
        taskbandProcess != GetCurrentProcessId() || !Current(request.target)) return;
    wchar_t name[64]{};
    if (!GetClassNameW(taskband, name, 64) || wcscmp(name, L"MSTaskSwWClass") != 0) return;
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto object = reinterpret_cast<const BYTE*>(GetWindowLongPtrW(taskband, 0));
    const BYTE* table = nullptr;
    if (!Read(object, table) || table != base + request.adapter.windowVtable) return;
    // Find the exact named COM subobject within the bounded native object.
    // No Windows-build offset, guessed vtable slot, or retained Shell pointer.
    const BYTE* manager = nullptr;
    for (size_t offset = 0; offset < 256; offset += sizeof(void*))
    {
        if (!Read(object + offset, table)) return;
        if (table == base + request.adapter.managerVtable)
        {
            if (manager) return;
            manager = object + offset;
        }
    }
    if (!manager) return;
    using Find = HRESULT(WINAPI*)(const void*, HWND, IUnknown**, IUnknown**);
    using Buttons = const Button32*(WINAPI*)(IUnknown*, int*);
    using Images = HIMAGELIST(WINAPI*)(IUnknown*);
    using Window = HWND(WINAPI*)(IUnknown*);
    const auto find = reinterpret_cast<Find>(const_cast<BYTE*>(base) + request.adapter.Get(Symbol::FindWindow).begin);
    Microsoft::WRL::ComPtr<IUnknown> group, item;
    if (FAILED(find(manager, reinterpret_cast<HWND>(request.target.window), &group, &item)) || !item)
    { request.error = ERROR_NOT_FOUND; return; }
    for (Symbol symbol : {Symbol::GetButtons, Symbol::GetImageList, Symbol::GetWindow})
        if (!HasMethod(item.Get(), base + request.adapter.Get(symbol).begin)) return;
    const auto window = reinterpret_cast<Window>(const_cast<BYTE*>(base) + request.adapter.Get(Symbol::GetWindow).begin);
    if (window(item.Get()) != reinterpret_cast<HWND>(request.target.window)) return;
    const auto buttons = reinterpret_cast<Buttons>(const_cast<BYTE*>(base) + request.adapter.Get(Symbol::GetButtons).begin);
    const auto images = reinterpret_cast<Images>(const_cast<BYTE*>(base) + request.adapter.Get(Symbol::GetImageList).begin);
    int count = 0;
    const auto source = buttons(item.Get(), &count);
    if (count < 0 || count > static_cast<int>(kMaximumButtons) || (count && !source)) return;
    request.snapshot.target = request.target;
    request.snapshot.identity = reinterpret_cast<std::uintptr_t>(item.Get());
    request.snapshot.count = static_cast<DWORD>(count);
    const HIMAGELIST imageList = images(item.Get());
    for (int index = 0; index < count; ++index)
    {
        auto& button = request.snapshot.buttons[index];
        if (!Read(source + index, button.native)) return;
        button.native.tip[259] = 0;
        if (button.native.id > 0xffff) return;
        HICON icon = nullptr;
        const auto& native = button.native;
        // The Shell prefers HICON, then falls back to the image list.
        if ((native.mask & THB_ICON) && native.icon) icon = CopyIcon(reinterpret_cast<HICON>(static_cast<UINT_PTR>(native.icon)));
        if (!icon && (native.mask & THB_BITMAP) && imageList && native.bitmap <= INT_MAX)
            icon = ImageList_GetIcon(imageList, static_cast<int>(native.bitmap), ILD_TRANSPARENT);
        if (icon)
        {
            tray::Event pixels{};
            tray::Pixels(icon, pixels);
            static_assert(kIconExtent == tray::kIconSize);
            button.width = pixels.width; button.height = pixels.height;
            button.pixels = pixels.pixels;
            DestroyIcon(icon);
        }
    }
    if (!Current(request.target)) return;
    request.error = ERROR_SUCCESS;
    if (!request.click) return;
    request.error = ERROR_NOT_FOUND;
    if (!request.expectedIdentity || request.expectedIdentity != request.snapshot.identity) return;
    for (DWORD index = 0; index < request.snapshot.count; ++index)
    {
        const auto& button = request.snapshot.buttons[index].native;
        if (button.id != request.button || !Interactive(button)) continue;
        // The documented per-window notification. No activation or global media key.
        const bool posted = PostMessageW(reinterpret_cast<HWND>(request.target.window),
            WM_COMMAND, MAKEWPARAM(button.id, THBN_CLICKED), 0) != FALSE;
        request.error = posted ? ERROR_SUCCESS : GetLastError();
        request.dismiss = posted && (button.flags & THBF_DISMISSONCLICK);
        return;
    }
}
DWORD GuardedCollect(HWND taskband, Request* request) noexcept
{
    __try { Collect(taskband, *request); return request->error; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return ERROR_INVALID_DATA; }
}
}
void Process(HWND taskband, DWORD owner, std::uint64_t nonce) noexcept
{
    try
    {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MappingName(owner, nonce).c_str());
        if (!mapping) return;
        auto* request = static_cast<Request*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
        if (request)
        {
            HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner);
            const bool valid = request->magic == kMagic && request->version == kVersion &&
                request->bytes == sizeof(Request) && request->owner == owner && process &&
                request->ownerCreation == hook_entry::ProcessCreation(process) &&
                WaitForSingleObject(process, 0) == WAIT_TIMEOUT && !request->complete;
            if (valid)
            {
                request->error = GuardedCollect(taskband, request);
                MemoryBarrier();
                InterlockedExchange(&request->complete, TRUE);
            }
            if (process) CloseHandle(process);
            UnmapViewOfFile(request);
        }
        CloseHandle(mapping);
    }
    catch (...) { /* Never unwind through the Explorer window thread. */ }
}
}
