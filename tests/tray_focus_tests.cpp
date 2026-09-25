#include <windows.h>
#include <commctrl.h>
#include "taskbar_hook/tray_protocol.h"
#include <iostream>
#include <memory>
#include <cstdlib>

namespace foreground_fixture
{
HWND current = nullptr;
bool permission = true, activation = true, nativeReturn = false, nativePermission = false;
HWND WINAPI Get() { return current; }
BOOL WINAPI Grant(DWORD) { return permission; }
BOOL WINAPI Set(HWND window)
{
    if (!activation) return FALSE;
    current = window; return TRUE;
}
}
// The inactive test desktop cannot own global foreground. Replace only those
// OS calls; compile the production collector and restore guard unchanged. Keep
// real HWND ownership/visibility, subclassing, worker and shared-memory queue.
#define GetForegroundWindow foreground_fixture::Get
#define AllowSetForegroundWindow foreground_fixture::Grant
#define SetForegroundWindow foreground_fixture::Set
#define RestoreFocus FixtureRestoreFocus
#define SnowDesktopTrayHookProc FixtureTrayHookProc
#include "tray_focus.h"
#include "../src/taskbar_hook/tray_collector.cpp"
#undef SnowDesktopTrayHookProc
#undef RestoreFocus
#undef SetForegroundWindow
#undef AllowSetForegroundWindow
#undef GetForegroundWindow
#include "status_bar_interaction.h"

namespace
{
void Require(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL tray focus: " << message << '\n'; std::exit(1); }
}
unsigned nativeCalls = 0;
LRESULT CALLBACK ShellFixture(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_COPYDATA)
    {
        ++nativeCalls;
        if (foreground_fixture::nativeReturn)
        {
            foreground_fixture::current = window;
            foreground_fixture::permission = foreground_fixture::nativePermission;
        }
        return TRUE;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

// Called after the menu test joins a private desktop, never SwitchDesktop.
// Shell's default handler, the icon app and global foreground are fixtures.
// This validates the collector chain, not real Explorer's foreground policy.
void RunTrayFocusWindowTests()
{
    using namespace snowdesktop;
    using namespace snowdesktop::tray;
    WNDCLASSW definition{};
    definition.hInstance = GetModuleHandleW(nullptr);
    definition.lpszClassName = L"Shell_TrayWnd"; definition.lpfnWndProc = ShellFixture;
    Require(RegisterClassW(&definition) != 0, "register isolated shell fixture");
    const auto shell = CreateWindowW(definition.lpszClassName, L"", WS_POPUP, 0, 60, 100, 30,
        nullptr, nullptr, definition.hInstance, nullptr);
    const auto bar = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP, 0, 0, 200, 32,
        nullptr, nullptr, definition.hInstance, nullptr);
    const auto app = CreateWindowW(L"STATIC", L"", WS_POPUP, 0, 100, 200, 100,
        nullptr, nullptr, definition.hInstance, nullptr);
    const auto other = CreateWindowW(L"STATIC", L"", WS_POPUP, 220, 100, 200, 100,
        nullptr, nullptr, definition.hInstance, nullptr);
    Require(shell && bar && app && other, "create only test-owned windows");
    for (const auto window : {bar, app, other}) ShowWindow(window, SW_SHOWNOACTIVATE);
    foreground_fixture::current = app;

    const DWORD pid = GetCurrentProcessId();
    const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedState)), ObjectName(pid, L"State").c_str());
    Require(mapping && GetLastError() != ERROR_ALREADY_EXISTS, "create unique collector mapping");
    const auto signal = CreateEventW(nullptr, FALSE, FALSE, ObjectName(pid, L"Signal").c_str());
    Require(signal != nullptr, "create collector signal");
    auto* state = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    Require(state != nullptr, "map collector state");
    new(state) SharedState(); state->owner = pid; state->epoch = 123;
    FILETIME creation{}, exit{}, kernel{}, user{};
    Require(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE, "read owner creation identity");
    state->ownerCreation = static_cast<std::uint64_t>(creation.dwHighDateTime) << 32 | creation.dwLowDateTime;
    CWPSTRUCT attach{}; attach.hwnd = shell;
    attach.message = RegisterWindowMessageW(kAttachMessage); attach.wParam = pid;
    FixtureTrayHookProc(HC_ACTION, 0, reinterpret_cast<LPARAM>(&attach));
    const auto readyDeadline = GetTickCount64() + 4000;
    while (!Read(state->ready) && GetTickCount64() < readyDeadline) WaitForSingleObject(signal, 50);
    Require(Read(state->ready) == 1, "real collector worker starts on isolated shell");

    FocusTicket ticket;
    ticket.epoch = 123; ticket.serial = 1; ticket.origin = reinterpret_cast<std::uint64_t>(bar);
    ticket.source = ticket.origin; ticket.started = GetTickCount(); ticket.keyboard = 1;
    ticket.identity = {{0x745231, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}}, reinterpret_cast<std::uint64_t>(app), 17, pid};
    WriteFocusTicket(*state, ticket);
    ShellTrayData wire{}; wire.operation = NIM_SETFOCUS;
    wire.icon.size = sizeof(NotifyIcon32); wire.icon.flags = NIF_GUID | NIF_ICON;
    wire.icon.guid = ticket.identity.guid; // GUID-only callers may omit HWND and ID.
    wire.icon.icon = 0xffffffffu; // Focus must not attempt to read this icon.
    const auto send = [&] {
        COPYDATASTRUCT copy{1, static_cast<DWORD>(sizeof(wire)), &wire};
        return SendMessageW(shell, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy));
    };
    Require(send() != FALSE, "production subclass accepts matching NIM_SETFOCUS");
    Event event;
    bool received = false;
    const auto deadline = GetTickCount64() + 4000;
    while (!(received = Consume(*state, event)) && GetTickCount64() < deadline) WaitForSingleObject(signal, 50);
    Require(received && event.operation == NIM_SETFOCUS && event.focusSerial == 1 &&
        event.epoch == 123 && event.focusForeground == reinterpret_cast<std::uint64_t>(app) &&
        SameFocusIdentity(event.identity, ticket.identity) && !event.width && !event.height && !nativeCalls,
        "production worker preserves identity, serial and foreground without copying icon pixels");
    FocusReturnTracker tracker; tracker.Arm(ticket, "player");
    Require(tracker.Arrive(event, 123, reinterpret_cast<std::uint64_t>(foreground_fixture::current)), "collector reply reaches host tracker");
    const auto delivery = tracker.Take(ticket.origin, 1, 123, reinterpret_cast<std::uint64_t>(app));
    Require(delivery && FixtureRestoreFocus(*delivery) && foreground_fixture::current == bar && GetFocus() == bar,
        "restore guard focuses the real no-activate HWND when the foreground boundary accepts it");
    Require(send() && nativeCalls == 1 && Read(state->read) == Read(state->write), "duplicate request retains native fallback without another reply");

    foreground_fixture::current = other;
    Require(!FixtureRestoreFocus(*delivery) && foreground_fixture::current == other, "delayed reply cannot steal foreground from another window");
    foreground_fixture::current = app;
    foreground_fixture::activation = false;
    Require(!FixtureRestoreFocus(*delivery) && foreground_fixture::current == app, "denied foreground permission cannot be forced");
    foreground_fixture::activation = true;
    ShowWindow(bar, SW_HIDE);
    Require(!FixtureRestoreFocus(*delivery) && foreground_fixture::current == app, "hidden bar is never resurrected by a reply");
    ++ticket.serial; WriteFocusTicket(*state, ticket);
    Require(send() && nativeCalls == 2 && Read(state->read) == Read(state->write), "Hook rejects a hidden return origin before queueing");
    ShowWindow(bar, SW_SHOWNOACTIVATE);
    WriteFocusTicket(*state, {});
    Require(send() && nativeCalls == 3, "blank dismissal cancels the shared request before it reaches the worker");
    WriteFocusTicket(*state, ticket); state->epoch = 124;
    Require(send() && nativeCalls == 4, "Hook rejects a return from a previous collector generation");
    state->epoch = 123; ++wire.icon.guid.Data1;
    Require(send() && nativeCalls == 5, "another icon cannot claim the pending focus request");

    --wire.icon.guid.Data1;
    ++ticket.serial; WriteFocusTicket(*state, ticket);
    foreground_fixture::permission = false;
    foreground_fixture::nativeReturn = true;
    foreground_fixture::nativePermission = true;
    Require(send() && nativeCalls == 6, "denied first grant invokes the native handler once");
    received = false;
    const auto fallbackDeadline = GetTickCount64() + 4000;
    while (!(received = Consume(*state, event)) && GetTickCount64() < fallbackDeadline) WaitForSingleObject(signal, 50);
    Require(received && event.focusSerial == ticket.serial &&
        event.focusForeground == reinterpret_cast<std::uint64_t>(shell),
        "native fallback queues a return only after the exact shell HWND receives foreground permission");
    foreground_fixture::current = app;
    foreground_fixture::permission = false;
    foreground_fixture::nativeReturn = false;
    ++ticket.serial; WriteFocusTicket(*state, ticket);
    Require(send() && nativeCalls == 7 && Read(state->read) == Read(state->write),
        "unsupported native return never claims success or posts a host focus event");

    StatusBarItem overflow{}; overflow.action = StatusBarAction::Tray; overflow.bounds = {80, 0, 112, 32};
    StatusBarItem pinned{}; pinned.action = StatusBarAction::Tray; pinned.bounds = {32, 0, 64, 32};
    pinned.icon.emplace(); pinned.icon->key = "player";
    std::vector<StatusBarItem> items{overflow, pinned};
    Require(FindStatusBarTrayFocus(items, "player") == 1, "pinned focus follows identity instead of a menu position");
    std::swap(items[0], items[1]);
    Require(FindStatusBarTrayFocus(items, "player") == 0, "reordering retains the same pinned target");
    items.erase(items.begin());
    Require(FindStatusBarTrayFocus(items, "player") == 0, "overflow returns to the existing tray entry without reopening it");
    items[0].bounds = {};
    Require(!FindStatusBarTrayFocus(items, "player"), "missing visible tray target never chooses an unrelated button");

    InterlockedExchange(&state->stop, 1);
    SendMessageW(shell, RegisterWindowMessageW(kDetachMessage), pid, 0);
    for (const auto window : {other, app, bar, shell}) DestroyWindow(window);
    UnregisterClassW(definition.lpszClassName, definition.hInstance);
    // The collector's independent mapping remains valid until its worker exits.
    UnmapViewOfFile(state); CloseHandle(mapping); CloseHandle(signal);
}
