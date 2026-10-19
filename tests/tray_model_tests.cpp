#include "system/tray/tray_service.h"
#include "system/tray/tray_order.h"
#include "system/tray/tray_menu_placement.h"
#include "system/status_bar/status_bar_notification.h"
#include "system/status_bar/status_bar_input_method.h"
#include "system/status_bar/status_bar_input_method_identity.h"
#include "dock/dock_tray_targets.h"
#include "dock/dock_tray_storage.h"
#include "dock/dock_tray_popup_layout.h"
#include "system/tray/tray_native_tooltip.h"
#include "system/tray/tray_reregistration.h"
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
        // Only this fixture HWND receives requests. Its real process identity
        // represents each recipient role; unrelated applications see no test.
        const HWND window = CreateWindowExW(0, L"STATIC", L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        const UINT message = RegisterWindowMessageW(L"SnowDesktop.Tray.Reregistration.Tests");
        const DWORD process = GetCurrentProcessId();
        const DWORD otherHost = process ^ 0x80000000u, otherExplorer = process ^ 0x40000000u;
        check(window && message, "recipient exclusion fixture is available");
        if (window && message)
        {
            MSG pending{};
            check(PostReregistration(window, otherHost, process, message) == ReregistrationPost::Skipped &&
                !PeekMessageW(&pending, window, message, message, PM_REMOVE),
                "Explorer receives no synthetic restart request even when it is not the host");
            check(PostReregistration(window, process, otherExplorer, message) == ReregistrationPost::Skipped &&
                !PeekMessageW(&pending, window, message, message, PM_REMOVE),
                "collector host receives no synthetic restart request");
            check(PostReregistration(window, otherHost, otherExplorer, message) == ReregistrationPost::Posted &&
                PeekMessageW(&pending, window, message, message, PM_REMOVE) && pending.message == message,
                "a recipient outside host and Explorer receives the real asynchronous message");
            check(PostReregistration(nullptr, otherHost, otherExplorer, message) == ReregistrationPost::Skipped,
                "missing recipient cannot be posted a re-registration request");
        }
        if (window) DestroyWindow(window);
    }
    {
        namespace popup = snowdesktop::dock_tray::popup;
        const std::wstring raw = L"QQ & literal\n声音: 关闭\n会话消息: 原始文本";
        check(popup::Label({}, L"My shortcut") == L"My shortcut" && popup::Label(raw, L"My shortcut") == raw,
            "missing tooltip uses the shortcut name while available multiline text remains verbatim");
        const RECT content{10, 20, 310, 260};
        const auto viewport = popup::Viewport(content, 52);
        const auto launch = popup::LaunchButton(content, 52, 14);
        const int heights[]{108, 108, 108};
        const auto maximum = popup::MaximumScroll(viewport, heights);
        check(maximum > 0 && popup::HitTest({20, 230}, viewport, launch, heights, 0, 8) == 3 &&
            popup::HitTest({20, 230}, viewport, launch, heights, maximum, 8) == 3,
            "launch stays independently clickable above a scrolling list of accounts");
        check(popup::HitTest({20, 125}, viewport, launch, heights, 0, 8) == -1 &&
            popup::HitTest({20, 213}, viewport, launch, heights, maximum, 8) == -1,
            "row gaps and the reserved footer gap cannot highlight or activate clipped accounts");
        check(popup::HitTest({20, 30}, viewport, launch, heights, 0, 8) == 0 &&
            popup::HitTest({20, 30}, viewport, launch, heights, maximum, 8) == 1,
            "stationary pointer hover follows the visible account after scrolling");
    }
    {
        Icon cached;
        cached.identity = {{}, 10, 1, 100}; cached.key = Key(cached.identity);
        cached.callback = WM_APP + 1; cached.version = 4; cached.tipKnown = true;
        cached.width = cached.height = 1; cached.pixels = {0xff112233}; cached.application = L"Example.exe";
        std::vector<Icon> icons{cached};
        Event fresh;
        fresh.operation = kBootstrapIcon; fresh.flags = NIF_TIP; fresh.identity = cached.identity;
        fresh.callback = cached.callback; fresh.version = cached.version;
        wcscpy_s(fresh.tip, L"Example & unchanged\n原始提示");
        check(RefreshNativeTooltip(icons, fresh) && icons[0].tip == fresh.tip &&
            icons[0].pixels == cached.pixels && icons[0].application == cached.application,
            "an explicit fresh native read reconciles stale empty text without replacing icon or program identity");
        check(!RefreshNativeTooltip(icons, fresh), "unchanged native tooltip does not publish another update");
        ++fresh.identity.process;
        check(!RefreshNativeTooltip(icons, fresh), "a replaced process cannot refresh a stale account label");
        fresh.identity = cached.identity; ++fresh.callback;
        check(!RefreshNativeTooltip(icons, fresh), "a different native callback cannot refresh an old registration");
        fresh.callback = cached.callback; fresh.tip[0] = 0;
        check(RefreshNativeTooltip(icons, fresh) && icons[0].tip.empty(), "a current explicitly empty native tip returns to the shortcut fallback");
        Event live = fresh; live.operation = NIM_MODIFY;
        wcscpy_s(live.tip, L"Newest notification");
        check(Apply(icons, live) && icons[0].tip == live.tip, "live notifications drained after a native read take precedence");
        icons.clear();
        check(!RefreshNativeTooltip(icons, fresh) && icons.empty(), "tooltip refresh cannot resurrect a removed icon");
    }
    {
        // Production selection consumes registered icons, not process counts
        check(CollectedRegistrationFlags(NIF_TIP | NIF_ICON, NIM_ADD, false) == NIF_ICON &&
            CollectedRegistrationFlags(NIF_TIP | NIF_ICON, NIM_ADD, true) == (NIF_TIP | NIF_ICON) &&
            CollectedRegistrationFlags(NIF_TIP | NIF_ICON, NIM_MODIFY, true) == (NIF_TIP | NIF_ICON),
            "a rejected duplicate ADD cannot advertise an authoritative empty tooltip over native bootstrap text");
        // or tooltip-derived account IDs. Two identical tips remain distinct.
        namespace rules = snowdesktop::dock_tray;
        check(rules::ResolveClick(true, 2, false, false) == rules::ClickAction::NormalDock,
            "a callable task window retains Dock activation and minimization despite tray accounts");
        check(rules::ResolveClick(false, 1, false, false) == rules::ClickAction::WaitForDoubleClick &&
            rules::ResolveClick(false, 1, false, true) == rules::ClickAction::ActivateTray,
            "one tray instance honors Dock double-click launch preference");
        check(rules::ResolveClick(false, 1, true, false) == rules::ClickAction::ActivateTray &&
            rules::ResolveClick(false, 1, true, true) == rules::ClickAction::Consume,
            "Dock single-click restore consumes the second half of its click pair");
        check(rules::ResolveClick(false, 2, false, false) == rules::ClickAction::OpenPicker &&
            rules::ResolveClick(false, 2, true, false) == rules::ClickAction::OpenPicker &&
            rules::ResolveClick(false, 2, false, true) == rules::ClickAction::Consume,
            "multiple accounts open the picker on the first click without double-click auto-selection");
        auto preferences = rules::Defaults();
        {
            const std::vector<std::wstring> payloads{L"D:\\Chat\\app\\Client.exe", L"D:\\Chat\\8.1\\Client.exe"};
            std::vector<DockTrayRule> launcherRules{{L"D:\\Chat\\Client.exe", true, true, false}};
            const auto* resolved = rules::TargetRule(launcherRules, L"D:\\Chat\\Client.exe", payloads[0], payloads);
            check(resolved && !resolved->doubleClick && !rules::TargetRule(launcherRules, L"D:\\Chat\\Client.exe", L"E:\\Other\\Client.exe", payloads),
                "a launcher path rule forwards its own gesture only to validated installed payloads");
            launcherRules.push_back({payloads[0], true, false, true});
            resolved = rules::TargetRule(launcherRules, L"D:\\Chat\\Client.exe", payloads[0], payloads);
            check(resolved && !resolved->enabled, "an explicitly disabled payload path overrides its enabled launcher");
        }
        check(rules::FindRule(preferences, L"D:\\Apps\\qq.EXE") != nullptr &&
            rules::FindRule(preferences, L"E:\\Weixin\\Weixin.exe") != nullptr &&
            rules::FindRule(preferences, L"C:\\Old\\WeChat.exe") != nullptr &&
            rules::FindRule(preferences, L"C:\\Feishu\\Feishu.exe") != nullptr,
            "default tray applications follow exact case-insensitive executable names across install paths");
        check(!rules::FindRule(preferences, L"D:\\Apps\\QQMusic.exe") &&
            rules::NormalizeTarget(L"*.exe", false).empty(), "name matching cannot select similarly named or wildcard programs");
        Snapshot snapshot;
        snapshot.connected = true;
        Icon first;
        first.identity = {{}, 10, 1, 100};
        first.key = Key(first.identity);
        first.callback = WM_USER + 1;
        first.application = L"D:\\Apps\\QQ.exe";
        first.tip = L"QQ & name\nunchanged\t123";
        Icon second = first;
        second.identity.window = 20;
        second.identity.id = 2;
        second.key = Key(second.identity);
        snapshot.icons = {first, second};
        auto targets = rules::CollectTargets(snapshot, preferences, L"C:\\QQ\\QQ.exe");
        check(targets.size() == 2 && targets[0].tip == first.tip && targets[1].tip == first.tip &&
            targets[0].key != targets[1].key, "same-tooltip accounts remain independent and raw tooltip text is preserved");
        preferences.push_back({first.application, true, false, false});
        check(!rules::FindRule(preferences, first.application)->enabled &&
            rules::CollectTargets(snapshot, preferences, first.application).empty(),
            "a disabled exact-path override takes precedence over an enabled default name");
        preferences = rules::Defaults();
        snapshot.icons[1].state = NIS_HIDDEN;
        check(rules::CollectTargets(snapshot, preferences, first.application).size() == 1, "hidden registered icons do not become selectable accounts");
        check(rules::IsCurrentTarget(snapshot, first), "a current registered icon can be selected");
        snapshot.icons[0].identity.process = 200;
        check(!rules::IsCurrentTarget(snapshot, first), "an icon owner replaced while a selector is open cannot receive the stale account command");
        snapshot.connected = false;
        check(rules::CollectTargets(snapshot, preferences, first.application).empty(), "a disconnected tray snapshot cannot intercept launching");
        JsonValue document;
        check(ParseJson("{}", document) && rules::DecodeRules(document, preferences) && preferences == rules::Defaults(),
            "old settings receive tray defaults once when the preference key is absent");
        check(ParseJson("{\"trayApplications\":[]}", document) && rules::DecodeRules(document, preferences) && preferences.empty(),
            "an explicitly empty saved list never restores defaults");
        check(ParseJson("{\"trayApplications\":[{\"executable\":\"QQ.exe\",\"matchPath\":false,\"enabled\":false,\"doubleClick\":false}]}", document) &&
            rules::DecodeRules(document, preferences) && preferences.size() == 1 && !preferences[0].enabled && !preferences[0].doubleClick,
            "saved disabled rules and single-click selection remain disabled after loading");
        check(ParseJson("{\"trayApplications\":[{\"executable\":\"QQ\",\"matchPath\":false,\"enabled\":true,\"doubleClick\":true}]}", document) &&
            !rules::DecodeRules(document, preferences), "malformed tray preferences are rejected rather than guessed");
    }
    {
        // Real Weixin trace: MODIFY/NIF_ICON alternates null and a normal
        // handle every ~500 ms. Do not confuse its blank frame with a failed
        // CopyIcon/GetIconInfo of a nonnull handle.
        ShellTrayData packet{};
        packet.operation = NIM_MODIFY;
        packet.icon.size = sizeof(NotifyIcon32);
        packet.icon.window = 42; packet.icon.id = 7;
        packet.icon.flags = NIF_MESSAGE | NIF_ICON;
        packet.icon.callback = WM_USER + 9;
        Notification notification;
        HICON handle = nullptr;
        check(Decode(&packet, sizeof(packet), notification, handle) && !handle && (notification.flags & kClearIcon),
            "an explicit null Shell icon becomes a clear-image event");
        Event blank; static_cast<Notification&>(blank) = notification;
        blank.identity.process = 99;
        std::vector<Icon> blinking;
        check(!Apply(blinking, blank) && blinking.empty(), "a blank partial MODIFY cannot manufacture an icon");
        Event bright = blank;
        bright.operation = NIM_ADD; bright.flags &= ~kClearIcon;
        bright.width = bright.height = 2;
        std::fill_n(bright.pixels.begin(), 4, 0xff00ff00u);
        check(Apply(blinking, bright), "the blinking icon registers through the real model");
        blinking.front().version = 4; blinking.front().persistentKey = "saved-pin";
        const auto initial = blinking.front();
        for (unsigned frame = 0; frame < 3; ++frame)
        {
            check(Apply(blinking, blank) && blinking.size() == 1 && blinking.front().width == 1 &&
                blinking.front().height == 1 && blinking.front().pixels == std::vector<std::uint32_t>{0} &&
                SameFocusIdentity(blinking.front().identity, initial.identity) && blinking.front().callback == initial.callback &&
                blinking.front().version == 4 && blinking.front().persistentKey == "saved-pin" && !(blinking.front().state & NIS_HIDDEN),
                "a blink-off frame paints transparent pixels without losing its pin, slot or callback");
            bright.operation = NIM_MODIFY;
            check(Apply(blinking, bright) && blinking.front().pixels == initial.pixels,
                "the following bright frame restores the original image");
        }
        packet.icon.icon = 1; packet.icon.flags |= kClearIcon;
        check(Decode(&packet, sizeof(packet), notification, handle) && handle && !(notification.flags & kClearIcon),
            "a sender cannot forge the collector's private clear flag for a nonnull icon");
        Event failed; static_cast<Notification&>(failed) = notification;
        failed.identity.process = 99;
        check(Apply(blinking, failed) && blinking.front().pixels == initial.pixels,
            "failed nonnull icon conversion preserves the last successful image");
        packet.icon.flags = NIF_TIP | kClearIcon; packet.icon.icon = 0;
        check(Decode(&packet, sizeof(packet), notification, handle) && !(notification.flags & kClearIcon),
            "a tooltip-only update never clears the image");
        Event initialBlank = blank; initialBlank.operation = NIM_ADD;
        blinking.clear();
        check(Apply(blinking, initialBlank) && blinking.size() == 1 && blinking.front().pixels == std::vector<std::uint32_t>{0},
            "an initially blank ADD keeps a transparent actionable registration");
    }
    {
        // Coalescing preserves independent monitors and rejects stale queued
        // messages after the overflow HWND closes/reopens.
        std::vector<std::pair<HWND, std::uint64_t>> posted;
        bool allowPost = true;
        ChangeNotifications changes([&](HWND window, std::uint64_t token) {
            if (!allowPost) return false;
            posted.emplace_back(window, token); return true;
        });
        const auto first = reinterpret_cast<HWND>(1), second = reinterpret_cast<HWND>(2);
        changes.Subscribe(first); changes.Subscribe(second);
        changes.Publish(); changes.Publish();
        check(posted.size() == 2, "a burst queues at most one update per tray presenter");
        const auto firstToken = posted[0].second, secondToken = posted[1].second;
        check(changes.Acknowledge(first, firstToken) && !changes.Acknowledge(first, firstToken),
            "each change ticket can be acknowledged only once");
        changes.Publish();
        check(posted.size() == 3 && posted.back().first == first,
            "one monitor's paint cannot consume another monitor's pending update");
        changes.Unsubscribe(second); changes.Subscribe(second); changes.Publish();
        check(posted.size() == 4 && !changes.Acknowledge(second, secondToken) &&
            changes.Acknowledge(second, posted.back().second), "reopened overflow ignores its old queued notification");
        changes.Unsubscribe(first); changes.Unsubscribe(second); changes.Publish();
        check(posted.size() == 4, "closed presenters stop receiving updates");
        changes.Subscribe(first); allowPost = false; changes.Publish();
        allowPost = true; changes.Publish();
        check(posted.size() == 5 && changes.Acknowledge(first, posted.back().second),
            "failed posting does not permanently suppress later image changes");
    }
    {
        namespace input = snowdesktop::status_bar_input_method;
        check(input::detail::LayoutLabel(L"Microsoft Pinyin", L"ZH") == L"拼" &&
            input::detail::LayoutLabel(L"微软拼音", L"ZH") == L"拼" &&
            input::detail::LayoutLabel(L"US keyboard", L"EN") == L"EN" &&
            input::detail::LayoutLabel(L"Other Chinese input program", L"ZH") == L"ZH",
            "the layout symbol identifies Pinyin without inventing that profile for other keyboards");
        TF_INPUTPROCESSORPROFILE first{}, second{};
        first.dwProfileType=second.dwProfileType=TF_PROFILETYPE_INPUTPROCESSOR;
        first.langid=second.langid=0x0804;
        first.guidProfile.Data1=1;second.guidProfile.Data1=2;
        check(!input::SameProfile(first,second)&&input::SameProfile(first,first),
            "input programs sharing a language are identified by their TSF profile rather than language alone");
        check(input::native_menu::MatchesModeButton(L"Input indicator English mode", L"Input indicator") &&
            input::native_menu::MatchesModeButton(L"Input indicator", L"Input indicator") &&
            !input::native_menu::MatchesModeButton(L"Input indicator settings", L"") &&
            !input::native_menu::MatchesModeButton(L"Input indicators", L"Input indicator") &&
            !input::native_menu::MatchesModeButton(L"Volume English mode", L"Input indicator"),
            "native input lookup rejects other generic tray buttons and an unavailable OS resource");
        // Foreground language and IME conversion are distinct. Query failures
        // must not turn a Chinese keyboard into a false Chinese-mode indicator.
        check(input::detail::Label(0x0804, L"ZH", true, IME_CMODE_NATIVE) == L"中" &&
            input::detail::Label(0x0804, L"ZH", false, IME_CMODE_NATIVE) == L"英" &&
            input::detail::Label(0x0804, L"ZH", true, 0) == L"英",
            "Chinese input label follows both open state and conversion mode");
        check(input::detail::Label(0x0804, L"ZH", {}, {}).empty() &&
            input::detail::Label(0x0804, L"ZH", true, {}).empty() &&
            input::detail::Label(0x0411, L"JA", true, IME_CMODE_NATIVE) == L"あ" &&
            input::detail::Label(0x0412, L"KO", false, 0) == L"A" &&
            input::detail::Label(0x0409, L"EN", {}, {}) == L"EN" &&
            input::detail::Label(0, L"", {}, {}).empty(),
            "failed mode or language sampling produces no replacement label or dash");
        input::Snapshot old;
        old.foreground = reinterpret_cast<HWND>(1); old.thread = 10; old.layout = reinterpret_cast<HKL>(0x0804);
        check(input::detail::Matches(old, old.foreground, old.thread, old.layout) &&
            !input::detail::Matches(old, reinterpret_cast<HWND>(2), old.thread, old.layout) &&
            !input::detail::Matches(old, old.foreground, 11, old.layout) &&
            !input::detail::Matches(old, old.foreground, old.thread, old.layout, reinterpret_cast<HWND>(3)) &&
            !input::detail::Matches(old, old.foreground, old.thread, reinterpret_cast<HKL>(0x0409)) &&
            !input::detail::Matches({}, nullptr, 0, nullptr),
            "foreground or layout changes invalidate pending samples instead of showing another app's mode");
        input::detail::DisplayCache display;
        check(display.Get({}, {}, false).label.empty(), "startup has no fabricated input mode");
        old.label = L"A";
        input::detail::DisplayCache layoutDisplay;
        auto layoutOnly = old; layoutOnly.label.clear(); layoutOnly.layoutLabel = L"拼";
        check(layoutDisplay.Get(layoutOnly, layoutOnly, false).layoutLabel == L"拼" &&
            layoutDisplay.Get(layoutOnly, layoutOnly, false).label.empty(),
            "an available keyboard layout is shown without inventing an unavailable conversion mode");
        auto withMode = layoutOnly; withMode.label = L"中";
        layoutDisplay.Get(withMode, withMode, false, 100);
        layoutDisplay.Get(withMode, withMode, false, 700);
        check(layoutDisplay.Get(layoutOnly, layoutOnly, false).label == L"中",
            "an unavailable conversion query retains the same target's last successful mode");
        auto newLayoutTarget = layoutOnly; newLayoutTarget.foreground = reinterpret_cast<HWND>(2);
        check(layoutDisplay.Get(newLayoutTarget, newLayoutTarget, false, 1000).label == L"中" &&
            layoutDisplay.Get(newLayoutTarget, newLayoutTarget, false, 1599).label == L"中",
            "a transient unavailable mode does not flash the language button off");
        check(layoutDisplay.Get(newLayoutTarget, newLayoutTarget, false, 2600).label == L"中" &&
            layoutDisplay.Get(newLayoutTarget, newLayoutTarget, false, 10000).label == L"中",
            "unavailable conversion reads after changing foreground never confirm removal of the mode button");
        auto childTarget = newLayoutTarget; childTarget.focus = reinterpret_cast<HWND>(3);
        check(layoutDisplay.Get(childTarget, childTarget, false, 11000).label == L"中",
            "non-editable child focus retains the last successful presentation without hiding the button");
        childTarget.label = L"英";
        check(layoutDisplay.Get(childTarget, childTarget, false, 11001).label == L"英",
            "a successfully sampled mode replaces the retained presentation immediately");
        auto englishLayout = childTarget;
        englishLayout.layout = reinterpret_cast<HKL>(0x0409);
        englishLayout.label = englishLayout.layoutLabel = L"EN";
        check(layoutDisplay.Get(englishLayout, englishLayout, false, 12000).label == L"英" &&
            layoutDisplay.Get(englishLayout, englishLayout, false, 12599).label == L"英",
            "a confirmed plain keyboard must settle before removing the conversion-mode button");
        layoutDisplay.Get({}, {}, false, 12600);
        check(layoutDisplay.Get(englishLayout, englishLayout, false, 13000).label == L"英",
            "an unavailable foreground sample interrupts rather than completes pending removal");
        layoutDisplay.Get(englishLayout, englishLayout, true, 13600);
        check(layoutDisplay.Get(englishLayout, englishLayout, false, 14000).label == L"英" &&
            layoutDisplay.Get(englishLayout, englishLayout, false, 14599).label == L"英" &&
            layoutDisplay.Get(englishLayout, englishLayout, false, 14600).label == L"EN",
            "menu focus interrupts removal and a continuous confirmed keyboard change eventually hides the mode button");
        check(layoutDisplay.Get(childTarget, childTarget, false, 14601).label == L"英",
            "switching back to a confirmed IME displays its mode without a reveal delay");
        check(display.Get(old, old, false).label == L"A", "first valid input mode is presented immediately");
        auto other = old; other.foreground = reinterpret_cast<HWND>(2); other.label = L"B";
        check(display.Get(old, other, false).label == L"A" &&
            display.Get({}, {}, false).label == L"A",
            "foreground and sampling gaps retain the last successful mode without accepting a mismatched sample");
        for (int i = 0; i < 10000; ++i) display.Get({}, {}, false);
        check(display.Get({}, {}, false).label == L"A", "unavailable samples never expire the last successful mode");
        check(display.Get(other, other, true).label == L"A" &&
            display.Get({}, {}, false).label == L"A",
            "menu focus and dismissal preserve the last successful mode");
        check(display.Get(other, other, false).label == L"B",
            "a valid new typing target replaces the retained mode without delay");
    }
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
        supplement.version = 4;
        check(!Apply(registrations, supplement) && registrations.size() == 1 && registrations.front().version == 3,
            "classic supplementation cannot duplicate or downgrade an authoritative GUID icon");
        registrations.front().tip.clear(); registrations.front().tipKnown = false;
        wcscpy_s(supplement.tip, L"QQ & original\nunchanged");
        check(Apply(registrations, supplement) && registrations.front().tip == L"QQ & original\nunchanged" &&
            registrations.front().version == 3 && HasGuid(registrations.front().identity.guid),
            "late native bootstrap fills missing tooltip before hover without replacing owner or version");
        Event emptyTip = registration; emptyTip.operation = NIM_MODIFY; emptyTip.flags = NIF_TIP; emptyTip.tip[0] = 0;
        check(Apply(registrations, emptyTip) && !Apply(registrations, supplement) && registrations.front().tip.empty(),
            "an explicitly empty live tooltip cannot be resurrected by a late supplement");
        registrations.clear();
        for (const DWORD protocol : {0u, 3u, 4u})
        {
            supplement.version = protocol;
            check(Apply(registrations, supplement), "cold classic icon enters without re-registration");
            const auto context = Callbacks(registrations.front(), Activation::ContextKeyboard, {-40, 60});
            if (protocol == 4)
                check(context.size() == 1 && HIWORD(context.front().lp) == 7 &&
                    LOWORD(context.front().lp) == WM_CONTEXTMENU && GET_X_LPARAM(context.front().wp) == -40 &&
                    GET_Y_LPARAM(context.front().wp) == 60,
                    "cold v4 system icon receives its ID and screen anchor instead of legacy parameters");
            else if (protocol == 3)
                check(context.size() == 1 && context.front().wp == 7 && context.front().lp == WM_CONTEXTMENU,
                    "cold v3 icon keeps legacy packing with semantic context notification");
            else
                check(context.size() == 2 && context.front().wp == 7 && context.front().lp == WM_RBUTTONDOWN &&
                    context.back().lp == WM_RBUTTONUP, "cold legacy icon retains the legacy context gesture");
            auto stale = supplement; stale.version = protocol == 4 ? 0u : 4u;
            check(!Apply(registrations, stale) && registrations.front().version == protocol,
                "a late duplicate bootstrap cannot replace the previously collected version");
            registrations.clear();
        }
        supplement.version = 5;
        check(!Apply(registrations, supplement) && registrations.empty(),
            "an unsupported classic record cannot manufacture a legacy actionable icon");
        supplement.version = 4;
        check(Apply(registrations, supplement) && Apply(registrations, registration) && registrations.size() == 1 &&
            registrations.front().version == 4 &&
            registrations.front().key == Key(registration.identity),
            "the wire GUID upgrades a provisional classic identity without losing its version or leaving a duplicate");
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

        // Captured 2026-09-28: a Qt context menu reports a frameless top-level
        // tool window without WS_POPUP, although it is both active and foreground
        // on the notification UI thread. Use its observed bounds/flags, not its
        // application or window class name, to protect this recognition gap.
        MenuPopupObservation topLevelMenu{};
        topLevelMenu.window = 14943030; topLevelMenu.process = 50648;
        topLevelMenu.thread = topLevelMenu.notificationThread = 50660;
        topLevelMenu.event = EVENT_OBJECT_SHOW; topLevelMenu.eventTime = 101;
        topLevelMenu.style = WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        topLevelMenu.extendedStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
        topLevelMenu.visible = topLevelMenu.targetRelated = true;
        topLevelMenu.foregroundWindow = topLevelMenu.activeWindow = topLevelMenu.window;
        topLevelMenu.bounds = {1834, 1085, 2044, 1177};
        const auto armTopLevel = [&](bool context) {
            placement.Arm(50648, {1858, 14}, {0, 32, 2048, 1152}, 100,
                {}, {1848, 0, 1880, 32}, context);
        };
        armTopLevel(true);
        const auto topLevelPosition = placement.Observe(topLevelMenu, 102);
        check(topLevelPosition && topLevelPosition->x == 1834 && topLevelPosition->y == 32,
            "a freshly shown active frameless tool menu without WS_POPUP follows the top tray icon");
        check(placement.Bindings()[0].window == topLevelMenu.window,
            "a recognized top-level tool menu supplies the same retention binding as a popup menu");
        armTopLevel(false);
        check(!placement.Observe(topLevelMenu, 102), "left clicks cannot relocate a non-popup tool window");
        armTopLevel(true); topLevelMenu.foregroundWindow = 8;
        check(!placement.Observe(topLevelMenu, 102), "an inactive non-popup tool window is not a context menu");
        armTopLevel(true); topLevelMenu.foregroundWindow = topLevelMenu.window;
        topLevelMenu.style |= WS_CAPTION;
        check(!placement.Observe(topLevelMenu, 102), "a titled tool window is never moved as a tray menu");
        armTopLevel(true); topLevelMenu.style &= ~static_cast<LONG_PTR>(WS_CAPTION);
        topLevelMenu.extendedStyle |= WS_EX_APPWINDOW;
        check(!placement.Observe(topLevelMenu, 102), "an application window is excluded even with active context evidence");

        // Host-side events from the failed candidate: SHOW arrives inactive,
        // then LOCATIONCHANGE 32 ms later has both foreground/active evidence.
        // An external observer sampled these states too late to expose the gap.
        const auto armDeferred = [&] {
            placement.Arm(50648, {2327, 26}, {0, 40, 2560, 1440}, 100,
                {}, {2310, 0, 2350, 40}, true);
        };
        topLevelMenu.extendedStyle &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW);
        topLevelMenu.bounds = {2298, 1356, 2561, 1471};
        topLevelMenu.activeWindow = topLevelMenu.foregroundWindow = 0;
        armDeferred();
        check(!placement.Observe(topLevelMenu, 102) && !placement.Bindings()[0].window,
            "a visible but not yet active tool menu is remembered without movement or retention");
        topLevelMenu.event = EVENT_OBJECT_LOCATIONCHANGE; topLevelMenu.eventTime = 133;
        topLevelMenu.activeWindow = topLevelMenu.foregroundWindow = topLevelMenu.window;
        const auto deferredPosition = placement.Observe(topLevelMenu, 134);
        check(deferredPosition && deferredPosition->x == 2297 && deferredPosition->y == 40,
            "a previously shown tool menu is anchored after its later activation and geometry event");
        check(placement.Bindings()[0].window == topLevelMenu.window,
            "retention starts only after the deferred tool menu is confirmed");
        armDeferred();
        check(!placement.Observe(topLevelMenu, 134), "late activation without a SHOW nomination cannot move an existing tool window");
        topLevelMenu.event = EVENT_OBJECT_SHOW; topLevelMenu.eventTime = 101;
        topLevelMenu.activeWindow = topLevelMenu.foregroundWindow = 0;
        placement.Observe(topLevelMenu, 102);
        topLevelMenu.event = EVENT_OBJECT_HIDE; topLevelMenu.eventTime = 110;
        placement.Observe(topLevelMenu, 111);
        topLevelMenu.event = EVENT_OBJECT_LOCATIONCHANGE; topLevelMenu.eventTime = 133;
        topLevelMenu.activeWindow = topLevelMenu.foregroundWindow = topLevelMenu.window;
        check(!placement.Observe(topLevelMenu, 134) && !placement.Bindings()[0].window,
            "hiding an unconfirmed menu discards its pending nomination before late activation");

        // The other captured tray menu uses WS_THICKFRAME for a border while
        // remaining an active frameless tool popup. Its top was -94 on a top
        // status bar. Keep its dimensions and open below the originating icon.
        MenuPopupObservation framedMenu{};
        framedMenu.window = 5772302; framedMenu.process = 40980;
        framedMenu.thread = framedMenu.notificationThread = 19772;
        framedMenu.event = EVENT_OBJECT_SHOW; framedMenu.eventTime = 101;
        framedMenu.style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_THICKFRAME;
        framedMenu.extendedStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_WINDOWEDGE;
        framedMenu.visible = framedMenu.targetRelated = true;
        framedMenu.foregroundWindow = framedMenu.activeWindow = framedMenu.window;
        framedMenu.bounds = {2263, -94, 2478, 166};
        const auto armFramed = [&](bool context) {
            placement.Arm(40980, {2263, 166}, {0, 40, 2560, 1440}, 100,
                {}, {2230, 155, 2275, 200}, context);
        };
        armFramed(true);
        const auto framedPosition = placement.Observe(framedMenu, 102);
        check(framedPosition && framedPosition->x == 2263 && framedPosition->y == 200,
            "an active frameless tool menu with WS_THICKFRAME opens fully below its top-edge icon");
        check(placement.Bindings()[0].window == framedMenu.window,
            "the corrected framed menu remains bound for tray retention");
        armFramed(false);
        check(!placement.Observe(framedMenu, 102), "a resizable popup from a left click is still excluded");
        armFramed(true); framedMenu.activeWindow = 8;
        check(!placement.Observe(framedMenu, 102), "proximity alone cannot move a resizable popup");
        armFramed(true); framedMenu.activeWindow = framedMenu.window;
        framedMenu.style |= WS_SYSMENU;
        check(!placement.Observe(framedMenu, 102), "a tool window with application system controls is excluded");
        armFramed(true); framedMenu.style &= ~static_cast<LONG_PTR>(WS_SYSMENU);
        framedMenu.extendedStyle |= WS_EX_TRANSPARENT;
        check(!placement.Observe(framedMenu, 102), "a transparent shadow cannot use active context evidence");

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
