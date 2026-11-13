#include "tray_douyin_hover.h"

#include <shellapi.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace snowdesktop::tray::first_show::douyin
{
namespace
{
// The native tray client uses a shared appbar helper and several distinct
// cursor queries. Scope the compatibility answers to the verified callers.
constexpr DWORD kImageTimestamp = 0x63086893;
constexpr DWORD kImageSize = 0x80e000;
constexpr DWORD kRememberPointer = 0x1b7d59;
constexpr DWORD kComparePointer = 0x1b7f1e;
constexpr DWORD kCheckIconBounds = 0x1b8079;
constexpr DWORD kTaskbarHelper = 0x2c9c85;
constexpr DWORD kCardPlacement = 0x1f2ff9;
constexpr DWORD kCardAvailableSize = 0x1f32d8;
constexpr ULONGLONG kLeaveDelayMs = 220;

using Cursor = decltype(&GetCursorPos);
using AppBar = decltype(&SHAppBarMessage);
struct State
{
    Request request;
    HWND presenter = nullptr;
    HWND popup = nullptr;
    DWORD host = 0;
    UINT_PTR timer = 0;
    ULONGLONG lastInside = 0;
};
thread_local State state;

struct Import
{
    void** slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;

    bool Exchange(void* expected, void* next) const
    {
        if (!slot) return false;
        DWORD previous = 0;
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous)) return false;
        const auto before = InterlockedCompareExchangePointer(slot, next, expected);
        DWORD unused = 0;
        VirtualProtect(slot, sizeof(*slot), previous, &unused);
        return before == expected;
    }
};

// Only one TRAY_CLASS thread is eligible. The originals never change while
// a wrapper may be running on another thread.
const Cursor originalCursor = &GetCursorPos;
const AppBar originalAppBar = &SHAppBarMessage;
Import cursorImport, appBarImport;

std::uintptr_t Rva(const void* address)
{
    return reinterpret_cast<std::uintptr_t>(address) -
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

void** FindImport(const char* library, const char* function)
{
    const auto image = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kImageTimestamp ||
        nt->OptionalHeader.SizeOfImage != kImageSize) return nullptr;
    const auto imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= kImageSize ||
        imports.Size > kImageSize - imports.VirtualAddress) return nullptr;
    const auto descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + imports.VirtualAddress);
    for (DWORD index = 0; index < imports.Size / sizeof(*descriptors); ++index)
    {
        const auto& descriptor = descriptors[index];
        if (!descriptor.Name || descriptor.Name >= kImageSize) break;
        if (_stricmp(reinterpret_cast<const char*>(image + descriptor.Name), library) != 0) continue;
        if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk ||
            descriptor.OriginalFirstThunk >= kImageSize || descriptor.FirstThunk >= kImageSize) return nullptr;
        const auto names = reinterpret_cast<const IMAGE_THUNK_DATA*>(image + descriptor.OriginalFirstThunk);
        const auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor.FirstThunk);
        const auto count = (kImageSize - std::max(descriptor.OriginalFirstThunk, descriptor.FirstThunk)) / sizeof(*names);
        for (std::size_t entry = 0; entry < count && names[entry].u1.AddressOfData; ++entry)
        {
            const auto name = names[entry].u1.AddressOfData;
            if (IMAGE_SNAP_BY_ORDINAL(name) || name >= kImageSize - sizeof(IMAGE_IMPORT_BY_NAME)) continue;
            const auto named = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + name);
            if (std::strcmp(reinterpret_cast<const char*>(named->Name), function) == 0)
                return reinterpret_cast<void**>(&slots[entry].u1.Function);
        }
    }
    return nullptr;
}

bool LivePresenter()
{
    DWORD pid = 0;
    return state.presenter && IsWindowVisible(state.presenter) &&
        GetWindowThreadProcessId(state.presenter, &pid) && pid == state.host;
}

bool OverHoverArea(POINT pointer)
{
    const auto& icon = state.request.icon;
    const auto root = GetAncestor(WindowFromPoint(pointer), GA_ROOT);
    if (PtInRect(&icon, pointer) && root == GetAncestor(state.presenter, GA_ROOT)) return true;
    RECT popup{};
    if (!state.popup || !IsWindowVisible(state.popup) || !GetWindowRect(state.popup, &popup)) return false;
    if (PtInRect(&popup, pointer) && root == state.popup) return true;
    const RECT bridge{std::max(icon.left, popup.left), std::min(icon.bottom, popup.bottom),
        std::min(icon.right, popup.right), std::max(icon.top, popup.top)};
    return PtInRect(&bridge, pointer);
}

BOOL WINAPI CursorForHover(LPPOINT pointer)
{
    const auto caller = Rva(_ReturnAddress());
    const auto result = originalCursor(pointer);
    if (!result || (caller != kRememberPointer && caller != kComparePointer && caller != kCheckIconBounds) ||
        !LivePresenter()) return result;
    const auto now = GetTickCount64();
    if (OverHoverArea(*pointer)) state.lastInside = now;
    if (now - state.lastInside <= kLeaveDelayMs) *pointer = state.request.anchor;
    return result;
}

UINT_PTR WINAPI AppBarForHover(DWORD message, PAPPBARDATA data)
{
    if (message == ABM_GETTASKBARPOS && data && data->cbSize >= sizeof(APPBARDATA) &&
        Rva(_ReturnAddress()) == kTaskbarHelper && LivePresenter())
    {
        void* frames[8]{};
        const auto count = CaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
        {
            const auto caller = Rva(frames[i]);
            if (caller != kCardPlacement && caller != kCardAvailableSize) continue;
            data->uEdge = state.request.preferAbove ? ABE_BOTTOM : ABE_TOP;
            data->rc = state.request.row;
            return TRUE;
        }
    }
    return originalAppBar(message, data);
}

void Disarm()
{
    if (!state.timer) return;
    KillTimer(nullptr, state.timer);
    state = {};
    appBarImport.Exchange(appBarImport.replacement, appBarImport.original);
    cursorImport.Exchange(cursorImport.replacement, cursorImport.original);
}

void CALLBACK CheckLifetime(HWND, UINT, UINT_PTR timer, DWORD)
{
    if (timer != state.timer) return;
    const auto now = GetTickCount64();
    POINT pointer{};
    if (GetCursorPos(&pointer) && OverHoverArea(pointer)) state.lastInside = now;
    if (!LivePresenter() || (!state.popup && now >= state.request.expires) ||
        (state.popup && (!IsWindowVisible(state.popup) || now - state.lastInside > 1500)))
        Disarm();
}
}

bool Arm(const Request& request, DWORD host)
{
    const auto presenter = reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.presenter));
    DWORD process = 0;
    if (!presenter || !GetWindowThreadProcessId(presenter, &process) || process != host ||
        GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) != DPI_AWARENESS_PER_MONITOR_AWARE)
        return false;
    Disarm();
    const auto cursorSlot = FindImport("USER32.dll", "GetCursorPos");
    const auto appBarSlot = FindImport("SHELL32.dll", "SHAppBarMessage");
    if (!cursorSlot || !appBarSlot) return false;
    // Resolve the system entry points directly so another module's slot
    // replacement cannot become the original or be overwritten.
    cursorImport = {cursorSlot, reinterpret_cast<void*>(originalCursor), reinterpret_cast<void*>(&CursorForHover)};
    appBarImport = {appBarSlot, reinterpret_cast<void*>(originalAppBar), reinterpret_cast<void*>(&AppBarForHover)};
    if (!cursorImport.Exchange(cursorImport.original, cursorImport.replacement)) return false;
    if (!appBarImport.Exchange(appBarImport.original, appBarImport.replacement))
    {
        cursorImport.Exchange(cursorImport.replacement, cursorImport.original);
        return false;
    }
    const auto timer = SetTimer(nullptr, 0, 100, CheckLifetime);
    if (!timer)
    {
        appBarImport.Exchange(appBarImport.replacement, appBarImport.original);
        cursorImport.Exchange(cursorImport.replacement, cursorImport.original);
        return false;
    }
    state = {request, presenter, nullptr, host, timer, GetTickCount64()};
    return true;
}

void Bind(HWND popup)
{
    if (state.timer && !state.popup) state.popup = popup;
}
}
