// Compile the actual window implementation with a controlled Win32 clipboard
// boundary. HWND dispatch, selection, history and notifications stay real;
// no test reads, empties or replaces the user's system clipboard.
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include <commctrl.h>
#include <dwrite.h>
#include <imm.h>
#include <windowsx.h>
#include <wrl/client.h>
#include "ui/input/text_input_window.h"
#include "ui/input/text_input_state.h"
#include "ui/input/private_pointer.h"
#include "ui/render/app_font.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace clipboard_boundary
{
enum class Failure { None, Open, Get, Size, Lock, Allocate, Empty, Publish, Close };
Failure failure = Failure::None;
HGLOBAL text = nullptr;
bool control = false;
bool unicodeAvailable = true;
unsigned opens = 0, writes = 0;

void Reset(std::wstring_view value = L"replacement")
{
    if (text) ::GlobalFree(text);
    failure = Failure::None; control = false; unicodeAvailable = true; opens = writes = 0;
    text = ::GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    auto* destination = static_cast<wchar_t*>(::GlobalLock(text));
    if (destination)
    {
        std::memcpy(destination, value.data(), value.size() * sizeof(wchar_t));
        destination[value.size()] = 0;
        ::GlobalUnlock(text);
    }
}
BOOL WINAPI Open(HWND) { ++opens; return failure != Failure::Open; }
BOOL WINAPI Close() { return failure != Failure::Close; }
HANDLE WINAPI Get(UINT format)
{ return format == CF_UNICODETEXT && unicodeAvailable && failure != Failure::Get ? text : nullptr; }
SIZE_T WINAPI Size(HGLOBAL memory)
{ return failure == Failure::Size ? 0 : ::GlobalSize(memory); }
LPVOID WINAPI Lock(HGLOBAL memory)
{ return failure == Failure::Lock ? nullptr : ::GlobalLock(memory); }
HGLOBAL WINAPI Allocate(UINT flags, SIZE_T bytes)
{ return failure == Failure::Allocate ? nullptr : ::GlobalAlloc(flags, bytes); }
BOOL WINAPI Empty()
{
    if (failure == Failure::Empty) return FALSE;
    if (text) ::GlobalFree(text);
    text = nullptr;
    return TRUE;
}
HANDLE WINAPI Publish(UINT format, HANDLE memory)
{
    if (format != CF_UNICODETEXT || failure == Failure::Publish) return nullptr;
    text = static_cast<HGLOBAL>(memory); ++writes; return memory;
}
BOOL WINAPI Available(UINT format) { return format == CF_UNICODETEXT && unicodeAvailable && text != nullptr; }
SHORT WINAPI KeyState(int key)
{ return key == VK_CONTROL && control ? static_cast<SHORT>(0x8000) : 0; }
}

// The production editor now reads the application's input facade. Keep the
// controlled Ctrl boundary at that call site; clipboard and HWND behavior
// below still compile the actual editor implementation.
namespace snowdesktop::private_pointer
{
SHORT ClipboardBoundaryKeyState(int key) noexcept
{ return clipboard_boundary::KeyState(key); }
}

#define OpenClipboard clipboard_boundary::Open
#define CloseClipboard clipboard_boundary::Close
#define GetClipboardData clipboard_boundary::Get
#define SetClipboardData clipboard_boundary::Publish
#define EmptyClipboard clipboard_boundary::Empty
#define IsClipboardFormatAvailable clipboard_boundary::Available
#define GlobalAlloc clipboard_boundary::Allocate
#define GlobalLock clipboard_boundary::Lock
#define GlobalSize clipboard_boundary::Size
#define KeyState ClipboardBoundaryKeyState
#include "../../src/ui/input/text_input_window.cpp"
#undef OpenClipboard
#undef CloseClipboard
#undef GetClipboardData
#undef SetClipboardData
#undef EmptyClipboard
#undef IsClipboardFormatAvailable
#undef GlobalAlloc
#undef GlobalLock
#undef GlobalSize
#undef KeyState

namespace
{
int clipboardFailures = 0, clipboardChanges = 0;
void ClipboardCheck(bool value, const char* message)
{
    if (!value) { ++clipboardFailures; std::cerr << "FAIL clipboard: " << message << '\n'; }
}
LRESULT CALLBACK ClipboardOwner(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_COMMAND && HIWORD(wp) == EN_CHANGE) ++clipboardChanges;
    return DefWindowProcW(window, message, wp, lp);
}
std::wstring InputValue(HWND window, bool password)
{
    wchar_t value[128]{};
    if (password) snowdesktop::text_input::CopySecret(window, value, std::size(value));
    else GetWindowTextW(window, value, static_cast<int>(std::size(value)));
    std::wstring result(value);
    SecureZeroMemory(value, sizeof(value));
    return result;
}
void PrepareSelection(HWND window)
{
    SetWindowTextW(window, L"alpha");
    SendMessageW(window, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(window, EM_SETSEL, 1, 4);
}
void InvokeClipboard(HWND window, bool cut, bool shortcut)
{
    clipboard_boundary::control = shortcut;
    SendMessageW(window, shortcut ? WM_KEYDOWN : cut ? WM_CUT : WM_PASTE,
        shortcut ? cut ? 'X' : 'V' : 0, 0);
    clipboard_boundary::control = false;
}
void CheckUntouched(HWND window, bool password, int previousChanges)
{
    DWORD start = 0, end = 0;
    SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    ClipboardCheck(InputValue(window, password) == L"alpha" && start == 1 && end == 4 &&
        clipboardChanges == previousChanges && !SendMessageW(window, EM_CANUNDO, 0, 0),
        "a failed clipboard operation preserves selected text, range, history and change notifications");
}
}

int RunTextInputClipboardTests()
{
    namespace input = snowdesktop::text_input;
    using clipboard_boundary::Failure;
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = ClipboardOwner;
    type.lpszClassName = L"SnowDesktop.ClipboardBoundaryTestOwner";
    RegisterClassW(&type);
    HWND owner = CreateWindowExW(0, type.lpszClassName, L"", WS_OVERLAPPED,
        -32000, -32000, 300, 100, nullptr, nullptr, type.hInstance, nullptr);
    ClipboardCheck(owner != nullptr, "create the native clipboard test owner");
    if (!owner) return clipboardFailures;
    for (const bool password : {false, true})
    {
        HWND window = CreateWindowExW(0, input::WindowClass(), L"alpha",
            WS_CHILD | (password ? ES_PASSWORD : 0), 0, 0, 200, 40, owner,
            reinterpret_cast<HMENU>(101), type.hInstance, nullptr);
        ClipboardCheck(window != nullptr, "create the actual shared input window");
        if (!window) continue;
        if (password)
        {
            clipboard_boundary::Reset(); PrepareSelection(window);
            const int before = clipboardChanges;
            SendMessageW(window, WM_COPY, 0, 0); SendMessageW(window, WM_CUT, 0, 0);
            CheckUntouched(window, true, before);
            ClipboardCheck(clipboard_boundary::opens == 0 && clipboard_boundary::writes == 0,
                "password copy/cut never even opens the controlled clipboard or publishes plaintext");
        }
        for (const bool shortcut : {false, true})
        {
            clipboard_boundary::Reset(); clipboard_boundary::unicodeAvailable = false;
            PrepareSelection(window); const int before = clipboardChanges;
            InvokeClipboard(window, false, shortcut);
            CheckUntouched(window, password, before);
        }
        for (const bool shortcut : {false, true})
        for (const auto failure : {Failure::Open, Failure::Get, Failure::Size, Failure::Lock})
        {
            clipboard_boundary::Reset(); clipboard_boundary::failure = failure;
            PrepareSelection(window); const int before = clipboardChanges;
            InvokeClipboard(window, false, shortcut);
            CheckUntouched(window, password, before);
        }
        if (!password)
        {
            for (const bool shortcut : {false, true})
            for (const auto failure : {Failure::Open, Failure::Allocate, Failure::Lock,
                Failure::Empty, Failure::Publish, Failure::Close})
            {
                clipboard_boundary::Reset(); clipboard_boundary::failure = failure;
                PrepareSelection(window); const int before = clipboardChanges;
                InvokeClipboard(window, true, shortcut);
                CheckUntouched(window, false, before);
            }
            clipboard_boundary::Reset(L"中文\r\n\t文字");
            PrepareSelection(window); int before = clipboardChanges;
            InvokeClipboard(window, false, true);
            ClipboardCheck(InputValue(window, false) == L"a中文  文字a" && clipboardChanges == before + 1,
                "successful Ctrl+V normalizes a single-line value and changes the selection exactly once");
            SendMessageW(window, WM_UNDO, 0, 0);
            ClipboardCheck(InputValue(window, false) == L"alpha", "successful paste remains undoable");
            clipboard_boundary::Reset(); PrepareSelection(window); before = clipboardChanges;
            InvokeClipboard(window, true, true);
            const auto* copied = static_cast<const wchar_t*>(::GlobalLock(clipboard_boundary::text));
            const bool published = copied && std::wstring(copied) == L"lph";
            if (copied) ::GlobalUnlock(clipboard_boundary::text);
            ClipboardCheck(InputValue(window, false) == L"aa" && clipboardChanges == before + 1 &&
                published && clipboard_boundary::writes == 1,
                "successful Ctrl+X publishes the exact selected text before one deletion");
            clipboard_boundary::Reset(L""); PrepareSelection(window); before = clipboardChanges;
            InvokeClipboard(window, false, false);
            ClipboardCheck(InputValue(window, false) == L"aa" && clipboardChanges == before + 1,
                "a successfully acquired empty Unicode string remains a valid replacement");
        }
        DestroyWindow(window);
    }
    if (clipboard_boundary::text) ::GlobalFree(clipboard_boundary::text);
    clipboard_boundary::text = nullptr; clipboard_boundary::failure = Failure::None;
    DestroyWindow(owner);
    return clipboardFailures;
}
