#include "tray_service.h"
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
    InterlockedIncrement(&state->geometrySequence);
    check(!LookupGeometry(*state, a, rect), "Explorer does not wait while the host writes geometry");
    return failures;
}
