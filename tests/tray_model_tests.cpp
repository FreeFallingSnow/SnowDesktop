#include "tray_service.h"
#include "tray_order.h"
#include <iostream>
#include <memory>
#include <windowsx.h>

int RunTrayModelTests()
{
    using namespace snowdesktop::tray;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAIL tray: " << message << '\n'; }
    };
    // Private payloads are untrusted. Truncation must not manufacture valid
    // GUIDs or version numbers from adjacent process memory.
    ShellTrayData wire{};
    wire.operation = NIM_ADD; wire.icon.size = sizeof(NotifyIcon32);
    wire.icon.window = 42; wire.icon.id = 7; wire.icon.flags = NIF_MESSAGE | NIF_TIP | NIF_GUID;
    wire.icon.guid = {0x1234, 2, 3, {4, 5, 6, 7, 8, 9, 10, 11}};
    wire.icon.callback = WM_APP + 9; wcscpy_s(wire.icon.tip, L"Dynamic icon");
    Notification notification; HICON copied = nullptr;
    check(Decode(&wire, sizeof(wire), notification, copied) && notification.identity.guid == wire.icon.guid &&
        notification.identity.window == 42 && notification.callback == WM_APP + 9 && std::wstring(notification.tip) == L"Dynamic icon",
        "valid GUID, owner, callback and tooltip survive the wire boundary");
    check(!Decode(&wire, offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, guid), notification, copied),
        "truncated GUID payload must be rejected");
    wire.icon.size = sizeof(NOTIFYICONDATAW);
    check(Decode(&wire, sizeof(wire), notification, copied), "native caller size may exceed serialized handle width");
    // The reported zero-icon failure used 1484-byte Windows packets. Their
    // known prefix must survive arbitrary trailer bytes; never interpret them.
    std::array<std::byte, 1484> extended;
    extended.fill(std::byte{0xa5});
    std::memcpy(extended.data(), &wire, sizeof(wire));
    check(Decode(extended.data(), extended.size(), notification, copied) &&
        notification.identity.guid == wire.icon.guid && notification.identity.window == 42 &&
        notification.callback == WM_APP + 9 && std::wstring(notification.tip) == L"Dynamic icon",
        "extended Explorer packets retain icon identity, callback and tooltip");
    check(!Decode(extended.data(), kMaxNotificationBytes + 1, notification, copied),
        "oversized packets are rejected before reading any data");
    check(!Decode(extended.data(), offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, guid), notification, copied),
        "a declared GUID still requires the complete known prefix");

    std::vector<Icon> icons;
    Event event; static_cast<Notification&>(event) = notification;
    event.operation = NIM_ADD; event.identity.process = 99;
    check(Apply(icons, event) && icons.size() == 1, "add creates one icon");
    event.identity.window = 100;
    check(Apply(icons, event) && icons.size() == 1 && icons.front().identity.window == 100,
        "GUID re-registration replaces a stale HWND without duplicating the application");
    event.operation = NIM_SETVERSION; event.version = 4; Apply(icons, event);
    event.operation = NIM_MODIFY; event.flags = NIF_STATE; event.stateMask = NIS_HIDDEN; event.state = NIS_HIDDEN;
    Apply(icons, event);
    check(icons.front().version == 4 && icons.front().tip == L"Dynamic icon" && (icons.front().state & NIS_HIDDEN),
        "partial hidden-state updates preserve callback version and tooltip");
    const auto v4 = Callbacks(icons.front(), Activation::Keyboard, {-1900, -30});
    check(v4.size() == 1 && LOWORD(v4[0].lp) == NIN_KEYSELECT && HIWORD(v4[0].lp) == 7 &&
        GET_X_LPARAM(v4[0].wp) == -1900 && GET_Y_LPARAM(v4[0].wp) == -30,
        "v4 keyboard callback packs icon ID and signed screen anchor");
    icons.front().version = 0; icons.front().identity.id = 0x12345678;
    const auto old = Callbacks(icons.front(), Activation::RightUp, {40, 50});
    check(old.size() == 1 && old[0].wp == 0x12345678 && old[0].lp == WM_RBUTTONUP,
        "legacy callback retains its full 32-bit icon ID");
    for (const auto activation : {Activation::Keyboard, Activation::ContextKeyboard})
    {
        const auto legacy = Callbacks(icons.front(), activation, {-1900, -30});
        check(legacy.size() == 2 && legacy[0].wp == 0x12345678 && legacy[1].wp == 0x12345678 &&
            legacy[0].lp == WM_RBUTTONDOWN && legacy[1].lp == WM_RBUTTONUP,
            "legacy keyboard activation and context menus receive the complete documented right-button gesture");
    }
    icons.front().version = NOTIFYICON_VERSION;
    const auto v3Context = Callbacks(icons.front(), Activation::ContextKeyboard, {-1900, -30});
    const auto v3Select = Callbacks(icons.front(), Activation::Keyboard, {-1900, -30});
    check(v3Context.size() == 1 && v3Context[0].wp == 0x12345678 && v3Context[0].lp == WM_CONTEXTMENU &&
        v3Select.size() == 1 && v3Select[0].wp == 0x12345678 && v3Select[0].lp == NIN_KEYSELECT,
        "version 3 distinguishes keyboard activation from context menus without truncating its icon ID");
    icons.front().version = NOTIFYICON_VERSION_4;
    const auto v4Context = Callbacks(icons.front(), Activation::ContextKeyboard, {-1900, -30});
    check(v4Context.size() == 1 && HIWORD(v4Context[0].lp) == 0x5678 && LOWORD(v4Context[0].lp) == WM_CONTEXTMENU &&
        GET_X_LPARAM(v4Context[0].wp) == -1900 && GET_Y_LPARAM(v4Context[0].wp) == -30,
        "version 4 context menu receives one packed callback at its actual signed anchor");
    for (const DWORD version : {NOTIFYICON_VERSION, NOTIFYICON_VERSION_4})
    {
        icons.front().version = version;
        const auto down = Callbacks(icons.front(), Activation::RightDown, {-1900, -30});
        const auto up = Callbacks(icons.front(), Activation::RightUp, {-1900, -30});
        check(down.size() == 1 && LOWORD(down[0].lp) == WM_RBUTTONDOWN && up.size() == 2 &&
            LOWORD(up[0].lp) == WM_RBUTTONUP && LOWORD(up[1].lp) == WM_CONTEXTMENU,
            "new-version mouse menus retain raw callbacks before the semantic context callback");
    }
    {
        snowdesktop::StatusBarSettings settings;
        settings.trayOrder = {"offline", "app-a", "app-b"};
        settings.pinnedTrayItems = {"app-a"};
        Snapshot state; Icon first, second;
        first.key = "live-a"; first.persistentKey = "app-a";
        second.key = "live-b"; second.persistentKey = "app-b";
        state.icons = {first, second};
        check(PlaceIcon(settings, state, "live-b", true, "live-a") &&
            settings.trayOrder == std::vector<std::string>({"offline", "app-b", "app-a"}) &&
            settings.pinnedTrayItems.size() == 2, "bar drop pins before a live icon and preserves offline identities");
        check(PlaceIcon(settings, state, "live-a", false) && settings.pinnedTrayItems == std::vector<std::string>({"app-b"}),
            "overflow drop unpins without deleting order preferences");
        const auto saved = settings.trayOrder;
        check(!PlaceIcon(settings, state, "deleted", true) && settings.trayOrder == saved,
            "a removed icon cannot be persisted by a stale drag");
        state.icons.front().state = NIS_HIDDEN;
        check(!PlaceIcon(settings, state, "live-a", true), "hidden icons reject stale drag commits");
        Icon system;
        for (DWORD value = 0x7820ae73; value <= 0x7820ae75; ++value)
        {
            system.identity.guid = {value, 0x23e3, 0x4229, {0x82, 0xc1, 0xe4, 0x1c, 0xb6, 0x7d, 0x5b, 0x9c}};
            check(DuplicatesControlCenter(system), "known system controls are omitted from tray presentation");
            system.identity.guid.Data4[7] ^= 1;
            check(!DuplicatesControlCenter(system), "a third-party lookalike GUID is not hidden");
        }
    }
    event.operation = NIM_DELETE; Apply(icons, event);
    event.operation = NIM_SETVERSION;
    check(!Apply(icons, event) && icons.empty(), "late version updates cannot resurrect deleted icons");
    Identity a{}, b{}; a.window = b.window = 40; a.id = b.id = 2; a.process = 10; b.process = 11;
    check(!SameIdentity(a, b) && Key(a) != Key(b), "reused window and icon IDs from another process do not collide");

    auto state = std::make_unique<SharedState>();
    event.epoch = 123;
    for (std::size_t i = 0; i < kCapacity - 1; ++i) { event.identity.id = static_cast<DWORD>(i); check(Publish(*state, event), "bounded queue accepts available slots"); }
    check(!Publish(*state, event) && Read(state->resync) == 1, "overflow requests resynchronization instead of blocking Explorer");
    for (std::size_t i = 0; i < kCapacity - 1; ++i)
    {
        Event received;
        check(Consume(*state, received) && received.identity.id == i && received.epoch == 123,
            "queue retains event order and generation");
    }
    check(!Consume(*state, event), "empty queue never returns stale data");
    state->geometryCount = 1; state->geometries[0] = {a, {-1920, -50, -1890, -20}};
    RECT rect{};
    check(LookupGeometry(*state, a, rect) && rect.left == -1920 && rect.bottom == -20,
        "native menu lookup uses actual signed screen rectangle");
    const auto origin = GeometryReply(1, rect), extent = GeometryReply(2, rect);
    check(GET_X_LPARAM(origin) == -1920 && GET_Y_LPARAM(origin) == -50 &&
        LOWORD(extent) == 30 && HIWORD(extent) == 30,
        "Shell rectangle lookup returns signed origin followed by width and height, not the second corner");
    InterlockedIncrement(&state->geometrySequence);
    check(!LookupGeometry(*state, a, rect), "Explorer does not wait while the host writes geometry");

    // NIM_SETFOCUS carries identity only, never changes the icon model. Test
    // GUID-only callers and truncated legacy data without reading image bytes.
    wire.operation = NIM_SETFOCUS;
    wire.icon.flags = NIF_GUID | NIF_ICON | NIF_TIP | NIF_STATE;
    wire.icon.window = 0; wire.icon.icon = 0xffffffffu;
    check(Decode(&wire, sizeof(wire), notification, copied) && !copied &&
        notification.flags == NIF_GUID && !notification.tip[0] && !notification.callback,
        "focus requests cannot copy stale icons or mutate notification fields");
    check(!Decode(&wire, offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, guid), notification, copied),
        "GUID focus requests require a complete GUID");
    wire.icon.flags = NIF_STATE | NIF_ICON; wire.icon.window = 40; wire.icon.id = 2;
    check(Decode(&wire, offsetof(ShellTrayData, icon) + offsetof(NotifyIcon32, tip), notification, copied) &&
        !copied && notification.flags == 0, "legacy focus reads identity without requiring irrelevant state bytes");

    FocusTicket ticket;
    ticket.epoch = 123; ticket.serial = 9; ticket.origin = 70; ticket.source = 71;
    ticket.identity = a; ticket.started = 100;
    state->epoch = 123;
    WriteFocusTicket(*state, ticket);
    FocusTicket readTicket;
    check(ReadFocusTicket(*state, a, readTicket) && readTicket.serial == 9 &&
        !ReadFocusTicket(*state, b, readTicket), "only the active icon incarnation can request focus");
    check(ClaimFocusTicket(*state, ticket) && !ClaimFocusTicket(*state, ticket), "a focus ticket can be claimed only once");
    state->epoch = 124;
    check(!ReadFocusTicket(*state, a, readTicket), "reconnection rejects a ticket from the old generation");
    state->epoch = 123; InterlockedIncrement(&state->focusSequence);
    check(!ReadFocusTicket(*state, a, readTicket), "Explorer never spins while focus state is being changed");
    InterlockedIncrement(&state->focusSequence);
    ticket.identity.guid = wire.icon.guid; WriteFocusTicket(*state, ticket);
    Identity guidOnly{}; guidOnly.guid = wire.icon.guid;
    check(ReadFocusTicket(*state, guidOnly, readTicket), "GUID-only NIM_SETFOCUS finds the registered icon");
    guidOnly.window = a.window; guidOnly.process = b.process;
    check(!ReadFocusTicket(*state, guidOnly, readTicket), "GUID focus cannot name a reused HWND from another process");

    FocusReturnTracker focus;
    const auto reply = [&] {
        Notification result; result.operation = NIM_SETFOCUS; result.epoch = ticket.epoch;
        result.identity = ticket.identity; result.focusSerial = ticket.serial; result.focusForeground = 40;
        return result;
    };
    auto focusEvent = reply();
    focus.Arm(ticket, "player");
    check(!focus.Arrive(focusEvent, 124, 40) && !focus.Arrive(focusEvent, 123, 50),
        "wrong generation or changed foreground cannot deliver a return");
    auto replaced = focusEvent; ++replaced.identity.window;
    check(!focus.Arrive(replaced, 123, 40), "a re-registered GUID cannot consume a previous HWND's return");
    check(focus.Arrive(focusEvent, 123, 40) && !focus.Arrive(focusEvent, 123, 40), "valid reply is delivered once");
    check(!focus.Take(71, 9, 123, 40) && !focus.Take(70, 8, 123, 40) && focus.Current(),
        "old window messages cannot consume a newer pending return");
    const auto delivered = focus.Take(70, 9, 123, 40);
    check(delivered && delivered->key == "player" && !focus.Current() && !focus.Take(70, 9, 123, 40),
        "UI consumes one return to the original bar, not a reopened popup");
    focus.Arm(ticket, "player"); focus.Arrive(focusEvent, 123, 40);
    check(!focus.Take(70, 9, 123, 50) && !focus.Current(), "foreground switching after delivery rejects and clears the request");
    focus.Arm(ticket, "player"); focus.Arrive(focusEvent, 123, 40);
    check(!focus.Take(70, 9, 124, 40), "reconnection between posting and dispatch also rejects the return");
    focus.Arm(ticket, "player");
    check(!focus.ObserveForeground(50, 30, 99, false) &&
        !focus.ObserveForeground(70, 20, 101, false) &&
        !focus.ObserveForeground(71, 20, 102, false) &&
        !focus.ObserveForeground(41, a.process, 103, false) &&
        !focus.ObserveForeground(80, 30, 104, true),
        "old events, origin, source, owning app and the claimed native return preserve the active gesture");
    check(focus.ObserveForeground(50, 30, 105, false) && !focus.Arrive(focusEvent, 123, 40),
        "switching to another app cancels a pending request even if the user later returns");
    ticket.started = 0xfffffff0u; focus.Arm(ticket, "player");
    check(focus.ObserveForeground(50, 30, 5, false), "tick-count wrap does not disable foreground cancellation");
    ticket.started = 100; ++ticket.serial; focus.Arm(ticket, "new player");
    check(!focus.Arrive(focusEvent, 123, 40), "a new user gesture invalidates the prior reply");
    focus.Cancel();
    check(!focus.Arrive(reply(), 123, 40), "blank dismissal and hide cannot be undone by a queued return");
    return failures;
}
