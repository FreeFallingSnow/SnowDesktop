#include "tray_service.h"
#include "tray_order.h"
#include "tray_menu_placement.h"
#include "status_bar_notification.h"
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
    {
        // Status-only notification sampling must distinguish unavailable data,
        // an actual zero, and Windows 11 totals from Windows 10 unread badges.
        // Use the production decoder/cache with synthetic native query replies;
        // this test never requests access to the machine's notifications.
        namespace notice = snowdesktop::status_bar_notification;
        const notice::detail::WordReply off{0, 4, 0}, priority{0, 4, 1}, alarms{0, 4, 2}, count{0, 4, 7};
        const auto disabled = notice::detail::Decode(10, 19045, off, off);
        check(disabled.quiet == false && disabled.unreadCount == 0u && !disabled.totalCount,
            "valid off and zero notification values remain known rather than unavailable");
        const auto win10 = notice::detail::Decode(10, 19045, priority, count);
        const auto win11 = notice::detail::Decode(10, 26100, alarms, count);
        check(win10.quiet == true && win10.unreadCount == 7u && !win10.totalCount &&
            win11.quiet == true && !win11.unreadCount && win11.totalCount == 7u,
            "active quiet profiles and total-versus-unread count meanings remain distinct");
        const auto unknownProfile = notice::detail::Decode(10, 26100, {0, 4, 3}, count);
        check(!unknownProfile.quiet && unknownProfile.totalCount == 7u,
            "an unknown active profile does not suppress a separately valid notification count");
        for (const notice::detail::WordReply invalid : {
            notice::detail::WordReply{-1, 4, 0}, notice::detail::WordReply{0, 0, 0}, notice::detail::WordReply{0, 8, 0}})
        {
            const auto unknown = notice::detail::Decode(10, 26100, invalid, invalid);
            check(!unknown.quiet && !unknown.unreadCount && !unknown.totalCount,
                "native failures, empty states and changed payload sizes never manufacture false or zero");
            const auto partial = notice::detail::Decode(10, 26100, off, invalid);
            check(partial.quiet == false && !partial.totalCount,
                "a failed count does not turn a valid quiet-off sample into unknown");
        }
        const auto unsupported = notice::detail::Decode(11, 30000, priority, count);
        check(!unsupported.quiet && !unsupported.unreadCount && !unsupported.totalCount,
            "an unsupported Windows family cannot inherit a guessed private-state mapping");
        notice::detail::Cache cache;
        unsigned reads = 0;
        auto next = win11;
        const auto read = [&] { ++reads; return next; };
        check(cache.Get(0, read).totalCount == 7u && reads == 1,
            "the first cache sample executes even when the controlled clock starts at zero");
        next = {};
        check(cache.Get(4999, read).totalCount == 7u && reads == 1,
            "repeated monitor paints share the existing five-second sample");
        const auto failedRefresh = cache.Get(5000, read);
        check(reads == 2 && !failedRefresh.quiet && !failedRefresh.unreadCount && !failedRefresh.totalCount,
            "a failed refresh clears a successful cached value instead of reporting stale success");
        next = disabled;
        const auto restored = cache.Get(10000, read);
        check(reads == 3 && restored.quiet == false && restored.unreadCount == 0u && !restored.totalCount,
            "sampling recovers after failure and can report a newly valid zero");
    }
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

    {
        // A synthetic TaskbarCreated must not turn unrelated ADD failures into
        // success. Native existence and queue acceptance remain collector-side
        // checks; this policy bounds which registration may receive that ack.
        ReregisterSession session;
        Notification request;
        request.epoch = 30; request.operation = NIM_ADD; request.identity = a;
        request.flags = NIF_MESSAGE | NIF_ICON; request.callback = WM_APP + 3;
        check(!session.Eligible(request, 30, 100), "duplicate ADD cannot be acknowledged before a recovery session");
        check(!session.Arm(9, 30, 10, 30, 100) && !session.Arm(10, 29, 10, 30, 100),
            "only the current host and queue generation may arm re-registration");
        check(session.Arm(10, 30, 10, 30, 100) && session.Eligible(request, 30, 101),
            "a complete legacy-key registration is eligible during the explicit recovery window");
        check(!session.Eligible(request, 31, 101) && !session.Eligible(request, 30, 5100) &&
            !session.Eligible(request, 30, 99), "epoch changes, expiry and invalid time reject duplicate acknowledgements");
        auto invalid = request; invalid.epoch = 29;
        check(!session.Eligible(invalid, 30, 101), "an old notification cannot join a new recovery generation");
        invalid = request; invalid.flags |= NIF_GUID;
        check(!session.Eligible(invalid, 30, 101), "GUID lookup alone cannot prove the original callback owner");
        invalid = request; invalid.identity.guid.Data1 = 1;
        check(!session.Eligible(invalid, 30, 101), "a nonempty GUID cannot bypass owner proof by omitting its flag");
        invalid = request; invalid.operation = NIM_MODIFY;
        check(!session.Eligible(invalid, 30, 101), "recovery never rewrites a failed MODIFY result");
        invalid = request; invalid.flags = NIF_MESSAGE;
        check(!session.Eligible(invalid, 30, 101), "partial ADD cannot be acknowledged as a full registration");
        invalid = request; invalid.callback = 0;
        check(!session.Eligible(invalid, 30, 101), "a missing callback cannot be made actionable by recovery");
        check(ReregisterSession::SameOwner(a, 10, 20, 10, 20) &&
            !ReregisterSession::SameOwner(a, 10, 20, 11, 20) &&
            !ReregisterSession::SameOwner(a, 10, 20, 10, 21) &&
            !ReregisterSession::SameOwner(a, 10, 0, 10, 0),
            "a destroyed, recycled or different-thread owner invalidates native lookup evidence");
        check(session.Acknowledge(request, 30, 102) && !session.Acknowledge(request, 30, 103),
            "one icon cannot consume repeated duplicate ADD acknowledgements in the same session");
        check(session.Arm(10, 30, 10, 30, 104) && !session.Eligible(request, 30, 105) &&
            !session.Arm(10, 30, 10, 30, 5100),
            "repeated control messages cannot reset per-icon acknowledgement or extend the recovery deadline");
        auto other = request; ++other.identity.id;
        check(session.Eligible(other, 30, 104), "the same HWND's other icon retains its independent recovery slot");
        ++other.identity.window; other.identity.id = request.identity.id;
        check(session.Eligible(other, 30, 104), "another HWND with the same icon ID is a distinct registration");
        other = request; other.identity.process = 0;
        check(!session.Eligible(other, 30, 104), "a missing live process never qualifies for recovery");
        session.Arm(10, 31, 10, 31, 200); request.epoch = 31;
        check(session.Acknowledge(request, 31, 201), "a new explicit recovery round may restore the same icon again");
    }

    {
        std::vector<Icon> registrations;
        Event registration;
        registration.operation = NIM_ADD; registration.identity = {wire.icon.guid, 42, 7, 99};
        registration.flags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        registration.callback = WM_APP + 9; registration.width = registration.height = 1;
        registration.pixels[0] = 0xff112233; wcscpy_s(registration.tip, L"Original");
        check(Apply(registrations, registration), "registration fixture enters through the production model");
        Event version = registration; version.operation = NIM_SETVERSION; version.version = 4;
        Apply(registrations, version);
        check(Apply(registrations, registration) && registrations.front().version == 4,
            "duplicate ADD after TaskbarCreated cannot downgrade a live v4 callback");
        Event partial; partial.operation = NIM_MODIFY; partial.identity.guid = registration.identity.guid;
        partial.flags = NIF_TIP; wcscpy_s(partial.tip, L"Updated");
        check(Apply(registrations, partial) && SameFocusIdentity(registrations.front().identity, registration.identity) &&
            registrations.front().version == 4 && registrations.front().tip == L"Updated",
            "GUID-only modification keeps callback HWND, process, ID and version");
        partial.operation = NIM_SETVERSION; partial.version = 3;
        check(Apply(registrations, partial) && registrations.front().version == 3 &&
            registrations.front().identity.window == 42, "GUID-only version update preserves its registered owner");
        partial.identity.window = 400; partial.identity.process = 100;
        check(!Apply(registrations, partial) && registrations.front().identity.window == 42,
            "a stale owner cannot change a re-registered GUID version");
        partial.operation = NIM_DELETE;
        check(!Apply(registrations, partial) && registrations.size() == 1,
            "a stale explicit owner cannot delete another incarnation of a GUID");
        Event supplement = registration; supplement.operation = kBootstrapIcon; supplement.identity.guid = {};
        check(!Apply(registrations, supplement) && registrations.size() == 1 && registrations.front().version == 3,
            "classic supplementation cannot duplicate or downgrade an authoritative GUID icon");
        registrations.clear();
        check(Apply(registrations, supplement) && Apply(registrations, registration) && registrations.size() == 1 &&
            registrations.front().key == Key(registration.identity),
            "the wire GUID upgrades a provisional classic identity without leaving a duplicate");
        registrations.front().application = L"old process"; registrations.front().version = 4;
        Event replacement = registration; replacement.identity.window = 400; replacement.identity.process = 100;
        replacement.flags = NIF_TIP;
        check(Apply(registrations, replacement) && registrations.front().version == 0 &&
            !registrations.front().callback && registrations.front().pixels.empty() && registrations.front().application.empty(),
            "a new GUID owner cannot inherit stale callback, version, pixels or executable identity");
        registrations.clear(); partial = registration; partial.operation = NIM_MODIFY; partial.flags = NIF_TIP;
        check(!Apply(registrations, partial) && registrations.empty(), "an unknown partial MODIFY cannot manufacture an icon");
        partial.flags = registration.flags;
        check(Apply(registrations, partial) && registrations.size() == 1 && registrations.front().callback == WM_APP + 9,
            "complete MODIFY registration supplements an icon that existed before collector attachment");
        partial.operation = 0x53440002;
        check(!Apply(registrations, partial) && registrations.size() == 1,
            "unknown optional collector operation is ignored without changing the model");
    }
    {
        // Bounds correction must never become a general application-window
        // mover. Exercise the same candidate policy as the live WinEvent hook.
        MenuPlacementSession placement;
        const auto arm = [&] { placement.Arm(99, {-1870, 50}, {-1920, 32, 0, 1080}, 100); };
        MenuPopupObservation popup{1, 99, EVENT_OBJECT_SHOW, 101, WS_POPUP, WS_EX_TOOLWINDOW,
            {-1900, -260, -1700, 60}, true, false, true};
        popup.targetRelated = true; popup.owner = 7; popup.thread = 9;
        popup.style |= WS_BORDER; // A menu border is not an application caption.
        arm(); const auto corrected = placement.Observe(popup, 102);
        check(corrected && corrected->x == -1900 && corrected->y == 32,
            "a nearby new popup fits the clicked monitor work area with signed coordinates");
        popup.event = EVENT_OBJECT_LOCATIONCHANGE;
        check(!placement.Observe(popup, 103) && !placement.Observe(popup, 104),
            "repeated geometry while the same async correction is pending does not issue more moves");
        for (unsigned i = 0; i < 2; ++i)
        {
            popup.bounds = {-1900, 32, -1700, 352};
            check(!placement.Observe(popup, 105 + i * 2), "observed corrected bounds acknowledge the async placement");
            popup.bounds = {-1900, -260, -1700, 60};
            check(placement.Observe(popup, 106 + i * 2).has_value(), "a menu that later lays itself out again gets a bounded retry");
        }
        popup.bounds = {-1900, 32, -1700, 352}; placement.Observe(popup, 109);
        popup.bounds = {-1900, -260, -1700, 60};
        check(!placement.Observe(popup, 110), "a popup that fights placement is not moved indefinitely");
        check(placement.Bindings()[0].window == popup.window,
            "exhausting movement does not discard the concrete menu retention binding");
        auto secondPopup = popup; secondPopup.window = 2; secondPopup.event = EVENT_OBJECT_SHOW;
        check(placement.Observe(secondPopup, 111).has_value() && placement.Bindings()[1].window == 2,
            "a second real menu retains its own correction budget after the root exhausts its retries");
        arm();
        check(!placement.Observe(popup, 102), "location-only events cannot nominate existing windows");
        check(!placement.Bindings()[0].window, "an old popup cannot acquire retention from location alone");
        popup.event = EVENT_OBJECT_SHOW; popup.process = 100;
        check(!placement.Observe(popup, 102), "another application's popup is never corrected");
        popup.process = 99; popup.style |= WS_CAPTION;
        check(!placement.Observe(popup, 102), "a normal application window is never corrected");
        popup.style = WS_POPUP; popup.notificationWindow = true;
        check(!placement.Observe(popup, 102), "the notification owner HWND itself is never moved");
        popup.notificationWindow = false; popup.targetRelated = false;
        check(!placement.Observe(popup, 102), "an unrelated same-process tool window cannot be mistaken for a tray menu");
        popup.targetRelated = true; popup.bounds = {-900, -260, -700, 60};
        check(!placement.Observe(popup, 102), "a distant popup from the same process is not a gesture candidate");
        popup.bounds = {-1900, -260, -1700, 60}; popup.eventTime = 99;
        check(!placement.Observe(popup, 102), "events queued before the user gesture are ignored");
        popup.eventTime = 101;
        check(!placement.Observe(popup, 1600) && !placement.Active(1600), "placement expires after its short fixed lifetime");
        arm(); placement.Cancel();
        check(!placement.Observe(popup, 102), "cancellation prevents delayed popup movement");
        placement.Arm(99, {-1870, 50}, {-1920, 32, 0, 1080}, 0xfffffff0u); popup.eventTime = 3;
        check(placement.Observe(popup, 5).has_value(), "tick-count wrap preserves the bounded placement lifetime");

        // Some custom menu frameworks issue SHOW before final geometry and
        // have no WS_EX_TOOLWINDOW. Exercise the real event policy without
        // discovering or moving a third-party HWND.
        arm(); popup.eventTime = 101; popup.extendedStyle = 0;
        popup.bounds = {0, 0, 0, 0};
        check(!placement.Observe(popup, 102), "an unfinished menu SHOW is remembered without moving it");
        popup.event = EVENT_OBJECT_LOCATIONCHANGE; popup.bounds = {-1900, -260, -1700, 60};
        const auto lateLayout = placement.Observe(popup, 103);
        check(lateLayout && lateLayout->y == 32,
            "a related new custom menu's final geometry is corrected without requiring a tool-window flag");
        check(placement.Bindings()[0].window == popup.window,
            "SHOW followed by final geometry also supplies non-activating menu retention");
        popup.owner = 8;
        check(!placement.Observe(popup, 104), "an owner change invalidates a bound menu before movement");
        check(!placement.Bindings()[0].window, "an owner change also invalidates menu retention evidence");
        popup.owner = 7;
        check(!placement.Observe(popup, 105), "an invalidated HWND cannot rejoin with location-only events");
        arm(); popup.event = EVENT_OBJECT_SHOW; popup.bounds = {-1900, -1100, -1700, 60};
        const auto tall = placement.Observe(popup, 102);
        check(tall && tall->y == 32, "a taller-than-work-area menu exposes its top without being resized");
        arm(); popup.bounds = {-1920, -80, 0, 1080};
        check(!placement.Observe(popup, 102), "a fullscreen-sized borderless popup is not moved as a menu");
        placement.Arm(99, {-1870, 50}, {-1920, 0, 0, 1080}, 100, {-1920, 0, 0, 44});
        popup.bounds = {-1900, -260, -1700, 60};
        const auto belowOverlay = placement.Observe(popup, 102);
        check(belowOverlay && belowOverlay->y == 44,
            "an overlay status bar is excluded even when it does not reserve the monitor work area");
        popup.event = EVENT_OBJECT_HIDE; placement.Observe(popup, 103);
        popup.event = EVENT_OBJECT_LOCATIONCHANGE;
        check(!placement.Observe(popup, 104), "a hidden menu loses its location-change binding");
        check(!placement.Bindings()[0].window, "a hidden menu is absent from retained popup evidence");

        // Replay the observed ordering without recognizing a framework/class:
        // a transparent shadow appears first; the owner is initially invisible
        // above the screen, then SHOW supplies its already-correct placement.
        placement.Arm(99, {1690, 16}, {0, 32, 1920, 1080}, 100, {}, {1674, 0, 1706, 32});
        MenuPopupObservation observed{};
        observed.window = 10; observed.process = 99; observed.thread = 9;
        observed.event = EVENT_OBJECT_SHOW; observed.eventTime = 101;
        observed.style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        observed.extendedStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
        observed.visible = true; observed.targetRelated = true; observed.owner = 11;
        observed.bounds = {1466, -446, 1910, 233};
        check(!placement.Observe(observed, 102) && !placement.Bindings()[0].window,
            "an out-of-bounds transparent shadow cannot become the root menu or consume its budget");
        observed.window = 11; observed.owner = 0; observed.visible = false;
        observed.event = EVENT_OBJECT_LOCATIONCHANGE;
        observed.style &= ~static_cast<LONG_PTR>(WS_VISIBLE);
        observed.extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        observed.bounds = {1538, -398, 1838, 137};
        check(!placement.Observe(observed, 103), "pre-SHOW invisible owner geometry cannot nominate a menu");
        observed.event = EVENT_OBJECT_SHOW; observed.visible = true; observed.style |= WS_VISIBLE;
        observed.bounds = {1538, 32, 1838, 567};
        check(!placement.Observe(observed, 104) && placement.Bindings()[0].window == 11,
            "the visible root is retained without moving its already correct top-edge placement");
        observed.bounds = {1538, 500, 1838, 1035};
        const auto followedRoot = placement.Observe(observed, 105);
        check(followedRoot && followedRoot->x == 1538 && followedRoot->y == 32,
            "the same gesture-bound root follows a later jump to the bottom while preserving its already aligned x coordinate");
        check(!placement.Observe(observed, 106), "a repeated root SHOW coalesces the same pending destination");
        observed.event = EVENT_OBJECT_HIDE; observed.visible = false; placement.Observe(observed, 107);
        observed.event = EVENT_OBJECT_SHOW; observed.visible = true;
        check(!placement.Observe(observed, 108) && !placement.Bindings()[0].window,
            "a hidden ownerless root cannot reuse its old near-anchor evidence for a new distant SHOW");

        // Captured ownerless root geometry and active/foreground identity,
        // with controlled icon/work bounds. No application or framework names
        // participate in recognition; this evidence is context-gesture only.
        MenuPopupObservation activePopup{};
        activePopup.window = 463372; activePopup.process = 20392;
        activePopup.thread = activePopup.notificationThread = 6768;
        activePopup.event = EVENT_OBJECT_SHOW; activePopup.eventTime = 101;
        activePopup.style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        activePopup.extendedStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        activePopup.visible = true; activePopup.targetRelated = true;
        activePopup.foregroundWindow = activePopup.activeWindow = activePopup.window;
        activePopup.bounds = {1472, 634, 1772, 1169};
        const auto armContext = [&](bool context) {
            placement.Arm(20392, {1837, 137}, {0, 32, 1920, 1080}, 100,
                {}, {1816, 116, 1856, 156}, context);
        };
        armContext(true);
        const auto activeRoot = placement.Observe(activePopup, 102);
        check(activeRoot && activeRoot->x == 1472 && activeRoot->y == 156,
            "a fresh active foreground tool popup on the callback UI thread follows an explicit context gesture");
        armContext(false);
        check(!placement.Observe(activePopup, 102) && !placement.Bindings()[0].window,
            "left-click activation does not grant a distant ownerless tool window menu placement");
        armContext(true); activePopup.activeWindow = 0;
        check(!placement.Observe(activePopup, 102), "foreground alone without the candidate as GUI active is insufficient");
        armContext(true); activePopup.activeWindow = activePopup.window; activePopup.foregroundWindow = 8;
        check(!placement.Observe(activePopup, 102), "GUI active without current foreground is insufficient");
        armContext(true); activePopup.foregroundWindow = activePopup.window; activePopup.thread = 6769;
        check(!placement.Observe(activePopup, 102), "a foreground tool popup on another same-process UI thread is not the callback menu");
        armContext(true); activePopup.thread = activePopup.notificationThread;
        activePopup.extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
        check(!placement.Observe(activePopup, 102), "active context evidence cannot relocate a non-tool application popup");
        armContext(true); activePopup.extendedStyle |= WS_EX_TOOLWINDOW;
        activePopup.event = EVENT_OBJECT_LOCATIONCHANGE;
        check(!placement.Observe(activePopup, 102), "active context evidence still requires a new SHOW nomination");

        // A menu rooted at the old taskbar may still be wholly inside rcWork.
        // Only concrete owner/menu-thread evidence permits moving it from far
        // away; this is not a general same-process popup mover.
        const RECT icon{-1890, 36, -1850, 64};
        placement.Arm(99, {-1870, 50}, {-1920, 32, 0, 1080}, 100, {}, icon);
        popup.event = EVENT_OBJECT_SHOW; popup.bounds = {-420, 760, -220, 1080};
        popup.strongTargetRelated = false;
        check(!placement.Observe(popup, 102) && !placement.Bindings()[0].window,
            "a distant weakly related popup is not re-anchored merely because it shares the target thread");
        popup.strongTargetRelated = true;
        const auto anchored = placement.Observe(popup, 103);
        check(anchored && anchored->x == -1870 && anchored->y == 64,
            "a strongly identified root menu at the old bottom edge follows the top icon even when already on screen");
        popup.bounds = {-1870, 64, -1670, 384}; popup.event = EVENT_OBJECT_LOCATIONCHANGE;
        check(!placement.Observe(popup, 104), "a root menu already reasonably anchored is left in place");
        auto submenu = popup; submenu.window = 3; submenu.owner = popup.window;
        submenu.parentMenu = popup.window; submenu.strongTargetRelated = false;
        submenu.event = EVENT_OBJECT_SHOW; submenu.bounds = {-1670, 160, -1470, 480};
        check(!placement.Observe(submenu, 105) && placement.Bindings()[1].window == 3,
            "an owner-linked submenu away from the gesture preserves its side-by-side placement");
        submenu.event = EVENT_OBJECT_LOCATIONCHANGE; submenu.bounds = {-120, 900, 80, 1220};
        const auto submenuFit = placement.Observe(submenu, 106);
        check(submenuFit && submenuFit->x == -200 && submenuFit->y == 760,
            "an existing submenu is clamped at the screen edge without being re-anchored over its root");

        placement.Arm(99, {-1870, 50}, {-1920, 32, 0, 1080}, 100, {}, icon);
        popup.event = EVENT_OBJECT_SHOW; popup.bounds = {};
        check(!placement.Observe(popup, 102), "a root SHOW without usable geometry waits for layout");
        popup.bounds = {-1900, -260, -1700, 60};
        const auto secondShow = placement.Observe(popup, 103);
        check(secondShow && secondShow->x == -1900 && secondShow->y == 64,
            "a repeated SHOW supplies final geometry and a top-edge root opens below its icon");

        placement.Arm(99, {-1870, 1050}, {-1920, 32, 0, 1080}, 100, {}, {-1890, 1036, -1850, 1064});
        popup.bounds = {-420, 32, -220, 352};
        const auto aboveBottom = placement.Observe(popup, 102);
        check(aboveBottom && aboveBottom->x == -1870 && aboveBottom->y == 716,
            "a distant strong root follows a bottom icon by opening above it without changing size");
    }
    {
        MenuRetentionTracker menu;
        menu.Arm(100);
        check(menu.Retain(101, true, MenuForeground::Transition, false) && menu.Armed(),
            "WM_ACTIVATE without a next HWND cannot dismiss a tray callback before its menu opens");
        check(menu.Retain(120, true, MenuForeground::Origin, false) && menu.Armed(),
            "an asynchronous callback may still have the originating panel in foreground");
        check(menu.Retain(200, true, MenuForeground::TargetProcess, true) &&
            menu.Retain(60000, true, MenuForeground::TargetProcess, true),
            "an actual menu remains usable after the discovery deadline");
        check(!menu.Retain(60001, true, MenuForeground::TargetProcess, false) && !menu.Armed(),
            "closing the bound menu ends retention instead of keeping an arbitrary app window");
        menu.Arm(100); menu.Retain(200, true, MenuForeground::TargetProcess, true);
        check(menu.Retain(201, true, MenuForeground::Origin, false) && !menu.Armed(),
            "returning to the bar after a menu keeps the panel open without a stale retention ticket");
        menu.Arm(100); menu.Retain(200, true, MenuForeground::TargetProcess, true);
        check(menu.Retain(399, true, MenuForeground::Transition, false) &&
            !menu.Retain(400, true, MenuForeground::Transition, false),
            "a submenu handoff tolerates a short missing HWND without granting permanent retention");
        menu.Arm(100);
        check(!menu.Retain(1600, true, MenuForeground::TargetProcess, false),
            "an unconfirmed same-process window only receives bounded discovery grace");
        menu.Arm(100);
        check(!menu.Retain(101, true, MenuForeground::Unrelated, true),
            "switching to an unrelated app dismisses even while an old menu is visible");
        menu.Arm(100);
        check(!menu.Retain(101, false, MenuForeground::TargetProcess, true),
            "a destroyed notification target invalidates retained menu evidence");
        menu.Arm(0xfffffff0u);
        check(menu.Retain(3, true, MenuForeground::Transition, false) &&
            !menu.Retain(1484, true, MenuForeground::Transition, false),
            "retention grace remains finite across the tick-count wrap");
    }

    auto state = std::make_unique<SharedState>();
    event.epoch = 123; event.operation = kBootstrapIcon;
    for (std::size_t i = 0; i < kCapacity - 1; ++i) { event.identity.id = static_cast<DWORD>(i); check(Publish(*state, event), "bounded queue accepts available slots"); }
    check(!Publish(*state, event) && Read(state->resync) == 1, "overflow requests resynchronization instead of blocking Explorer");
    for (std::size_t i = 0; i < kCapacity - 1; ++i)
    {
        Event received;
        check(Consume(*state, received) && received.identity.id == i && received.epoch == 123 &&
            received.operation == kBootstrapIcon, "queue retains optional supplement operation, order and generation");
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
    auto geometryOwner = a; geometryOwner.guid = wire.icon.guid;
    state->geometries[0].identity = geometryOwner;
    Identity geometryRequest{}; geometryRequest.guid = wire.icon.guid;
    check(LookupGeometry(*state, geometryRequest, rect), "GUID-only native geometry query resolves the registered icon");
    geometryRequest.window = a.window; geometryRequest.process = b.process;
    check(!LookupGeometry(*state, geometryRequest, rect), "a reused owner cannot read another GUID incarnation's geometry");
    state->geometries[0].identity = a;
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
