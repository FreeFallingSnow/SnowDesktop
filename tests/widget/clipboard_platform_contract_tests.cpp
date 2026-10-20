// Compile the real executor once in this target, replacing only the Win32
// clipboard boundary. No user clipboard API is imported or called.
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "widget/tasks/widget_clipboard_task_executor.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
enum class Failure { None, Open, Empty, Publish, Close, Allocate, Lock, Owner };
struct ClipboardFixture
{
    Failure failure = Failure::None;
    HGLOBAL data = nullptr;
    HWND owner = nullptr;
    bool opened = false;
    bool validWorkerOwner = false;
    int opens = 0, empties = 0, publishes = 0, closes = 0;
} clipboardFixture;

void ResetFixture(Failure failure = Failure::None)
{
    if (clipboardFixture.data) GlobalFree(clipboardFixture.data);
    clipboardFixture = {};
    clipboardFixture.failure = failure;
    clipboardFixture.data = GlobalAlloc(GMEM_MOVEABLE, 12 * sizeof(wchar_t));
    auto* text = static_cast<wchar_t*>(GlobalLock(clipboardFixture.data));
    wcscpy_s(text, 12, L"old value");
    GlobalUnlock(clipboardFixture.data);
}
std::wstring FixtureText()
{
    if (!clipboardFixture.data) return {};
    auto* text = static_cast<const wchar_t*>(GlobalLock(clipboardFixture.data));
    std::wstring value(text);
    GlobalUnlock(clipboardFixture.data);
    return value;
}
BOOL WINAPI FixtureOpenClipboard(HWND owner)
{
    ++clipboardFixture.opens;
    if (clipboardFixture.failure == Failure::Open) return FALSE;
    clipboardFixture.owner = owner;
    clipboardFixture.opened = true;
    clipboardFixture.validWorkerOwner = owner && IsWindow(owner) &&
        GetWindowThreadProcessId(owner, nullptr) == GetCurrentThreadId();
    return TRUE;
}
BOOL WINAPI FixtureEmptyClipboard()
{
    ++clipboardFixture.empties;
    if (!clipboardFixture.opened || clipboardFixture.failure == Failure::Empty) return FALSE;
    if (clipboardFixture.data) GlobalFree(clipboardFixture.data);
    clipboardFixture.data = nullptr;
    return TRUE;
}
HANDLE WINAPI FixtureSetClipboardData(UINT format, HANDLE memory)
{
    ++clipboardFixture.publishes;
    // The actual Windows NULL-owner contract is the regression boundary.
    if (!clipboardFixture.opened || !clipboardFixture.validWorkerOwner ||
        format != CF_UNICODETEXT || clipboardFixture.failure == Failure::Publish)
        return nullptr;
    clipboardFixture.data = static_cast<HGLOBAL>(memory);
    return memory;
}
BOOL WINAPI FixtureCloseClipboard()
{
    ++clipboardFixture.closes;
    if (clipboardFixture.failure == Failure::Close && clipboardFixture.closes == 1) return FALSE;
    clipboardFixture.opened = false;
    return TRUE;
}
HANDLE WINAPI FixtureGetClipboardData(UINT format)
{ return format == CF_UNICODETEXT ? clipboardFixture.data : nullptr; }
BOOL WINAPI FixtureIsClipboardFormatAvailable(UINT format)
{ return format == CF_UNICODETEXT && clipboardFixture.data; }
HGLOBAL WINAPI FixtureGlobalAlloc(UINT flags, SIZE_T bytes)
{ return clipboardFixture.failure == Failure::Allocate ? nullptr : GlobalAlloc(flags, bytes); }
LPVOID WINAPI FixtureGlobalLock(HGLOBAL memory)
{ return clipboardFixture.failure == Failure::Lock ? nullptr : GlobalLock(memory); }
HWND WINAPI FixtureCreateWindowExW(DWORD extended, LPCWSTR className, LPCWSTR title, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter)
{
    return clipboardFixture.failure == Failure::Owner ? nullptr : CreateWindowExW(
        extended, className, title, style, x, y, width, height, parent, menu, instance, parameter);
}
}

#define OpenClipboard FixtureOpenClipboard
#define EmptyClipboard FixtureEmptyClipboard
#define SetClipboardData FixtureSetClipboardData
#define CloseClipboard FixtureCloseClipboard
#define GetClipboardData FixtureGetClipboardData
#define IsClipboardFormatAvailable FixtureIsClipboardFormatAvailable
#define GlobalAlloc FixtureGlobalAlloc
#define GlobalLock FixtureGlobalLock
#define CreateWindowExW FixtureCreateWindowExW
#include "../../src/widget/tasks/widget_clipboard_task_executor.cpp"
#undef OpenClipboard
#undef EmptyClipboard
#undef SetClipboardData
#undef CloseClipboard
#undef GetClipboardData
#undef IsClipboardFormatAvailable
#undef GlobalAlloc
#undef GlobalLock
#undef CreateWindowExW

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
snowdesktop::widget_runtime::WidgetClipboardTaskCompletion RunClipboard(
    std::string action, std::string text = {})
{
    using namespace snowdesktop::widget_runtime;
    WidgetClipboardTaskExecutor executor;
    const std::string format = action == "clipboard.clear" ? "" : "text";
    Require(static_cast<bool>(executor.Start(900, "platform-contract", {std::move(action), format, std::move(text)})),
        "real executor accepts the clipboard platform contract request");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto completed = executor.DrainCompletions();
        if (!completed.empty()) return std::move(completed.front());
        std::this_thread::yield();
    }
    Require(false, "real clipboard worker completes within the fixture deadline");
    return {};
}
}

int RunClipboardPlatformContractTests()
{
    ResetFixture();
    auto result = RunClipboard("clipboard.write", "Unicode \xe9\x9b\xaa / \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
    Require(result.ok && clipboardFixture.validWorkerOwner &&
            FixtureText() == L"Unicode 雪 / 日本語" && clipboardFixture.publishes == 1 &&
            !clipboardFixture.opened && !IsWindow(clipboardFixture.owner),
        "real worker writes Unicode through a valid owned HWND and closes before destroying it");
    ResetFixture();
    Require(RunClipboard("clipboard.write", "").ok && FixtureText().empty(),
        "the same production path intentionally publishes valid empty Unicode text");
    ResetFixture();
    Require(RunClipboard("clipboard.clear").ok && !clipboardFixture.data && clipboardFixture.validWorkerOwner,
        "clear also uses a valid worker-owned clipboard window");
    for (auto failure : {Failure::Allocate, Failure::Lock, Failure::Owner, Failure::Open, Failure::Empty})
    {
        ResetFixture(failure);
        Require(!RunClipboard("clipboard.write", "replacement").ok && FixtureText() == L"old value" &&
                clipboardFixture.publishes == 0,
            "failure before publication preserves the original clipboard fixture and reports failure");
    }
    ResetFixture(Failure::Publish);
    Require(!RunClipboard("clipboard.write", "replacement").ok && clipboardFixture.publishes == 1 &&
            !clipboardFixture.opened,
        "a rejected SetClipboardData is reported and the opened clipboard is closed");
    ResetFixture(Failure::Close);
    Require(!RunClipboard("clipboard.write", "replacement").ok && FixtureText() == L"replacement" &&
            !clipboardFixture.opened,
        "a close failure is not reported as successful publication and transferred memory remains owned");
    ResetFixture();
    result = RunClipboard("clipboard.read");
    Require(result.ok && result.text == "old value", "the actual read path retains its existing NULL-owner read semantics");
    GlobalFree(clipboardFixture.data);
    clipboardFixture.data = nullptr;
    return 0;
}
