#include "shell/shell_context_menu_invoke.h"
#include "shell/shell_start_pin.h"
#include "shell/shell_extension_menu.h"
#include "shell/shell_extension_catalogue.h"
#include "shell/shell_extension_attribution.h"
#include "shell/shell_extension_management.h"
#include "shell/shell_extension_menu_items.h"
#include "shell/shell_extension_menu_presentation.h"
#include "shell/shell_extension_nvidia_compat.h"
#include "ui/menu/menu_label.h"
#include "shell/shell_new_item_capture.h"
#include "shell/shell_popup_menu_tracker.h"
#include "ui/menu/modern_menu_appearance_rules.h"
#include "dock/floating_dock_rules.h"

#include <cstdlib>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#include <wrl/client.h>
#include <wrl/implements.h>

namespace
{

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestStartPinRouting()
{
    namespace pin = snowdesktop::shell_start_pin;
    // Only the native menu and Explorer execution are substituted. The real
    // router must preserve the chosen shortcut, dispatch once, and never send
    // another verb or invert an action whose state changed while the menu was open.
    class Menu final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IContextMenu>
    {
    public:
        std::wstring verb = L"PinToStartScreen";
        bool ansiOnly = false;
        IFACEMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return E_NOTIMPL; }
        IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO) override { return E_ACCESSDENIED; }
        IFACEMETHODIMP GetCommandString(UINT_PTR offset, UINT flags, UINT*, LPSTR output, UINT size) override
        {
            if (offset != 17 || !output || size <= verb.size()) return E_INVALIDARG;
            if (flags == GCS_VERBW && !ansiOnly)
            { wcscpy_s(reinterpret_cast<wchar_t*>(output), size, verb.c_str()); return S_OK; }
            if (flags == GCS_VERBA)
            { return WideCharToMultiByte(CP_ACP, 0, verb.c_str(), -1, output, static_cast<int>(size), nullptr, nullptr) ? S_OK : E_FAIL; }
            return E_NOTIMPL;
        }
    };
    auto context = Microsoft::WRL::Make<Menu>();
    unsigned calls = 0;
    const std::wstring shortcut = L"C:\\isolated\\微信.lnk";
    const auto execute = [&](pin::Action action, const std::wstring& path) {
        ++calls;
        Expect(action == pin::Action::Pin && path == shortcut,
            "Start pin dispatch preserves the selected Unicode shortcut rather than its executable target");
        return E_ACCESSDENIED;
    };
    const auto result = pin::Route(context.Get(), 17, {shortcut}, execute);
    Expect(result && *result == E_ACCESSDENIED && calls == 1,
        "a selected Start command routes to Explorer exactly once and keeps failure handled without local fallback");
    context->ansiOnly = true;
    context->verb = L"pintostartscreen";
    Expect(pin::Route(context.Get(), 17, {shortcut}, execute).has_value() && calls == 2,
        "legacy ANSI canonical verbs also route Start pinning without caption matching");
    Expect(pin::Route(context.Get(), 17, {}, execute) == E_INVALIDARG &&
        pin::Route(context.Get(), 17, {shortcut, L"C:\\other.lnk"}, execute) == E_INVALIDARG && calls == 2,
        "missing or ambiguous selections cannot pin an unrelated first item");
    context->ansiOnly = false;
    context->verb = L"open";
    Expect(!pin::Route(context.Get(), 17, {shortcut}, execute) && calls == 2,
        "ordinary and third-party Shell commands keep their existing local execution route");
    context->verb = L"UnpinFromStartScreen";
    Expect(pin::Route(context.Get(), 17, {shortcut}, [&](pin::Action action, const auto& path) {
        return action == pin::Action::Unpin && path == shortcut ? S_OK : E_FAIL;
    }) == S_OK, "unpin dispatch preserves the user's reverse action");
    struct Popup { HMENU value = CreatePopupMenu(); ~Popup() { DestroyMenu(value); } } popup;
    AppendMenuW(popup.value, MF_STRING, 18, L"A localized caption");
    Expect(!pin::FindCommand(context.Get(), popup.value, pin::Action::Pin) &&
        pin::FindCommand(context.Get(), popup.value, pin::Action::Unpin) == 17u,
        "a pin whose state became unpin is not executed as an accidental reversal");
    EnableMenuItem(popup.value, 18, MF_BYCOMMAND | MF_GRAYED);
    Expect(!pin::FindCommand(context.Get(), popup.value, pin::Action::Unpin),
        "Explorer rechecks current command availability before changing Start pins");

    pin::Request request;
    request.processId = 42;
    request.token = 123;
    wcscpy_s(request.path, shortcut.c_str());
    Expect(pin::ValidRequest(request, 42, 123), "a bound absolute Start request is valid");
    for (int mutation = 0; mutation < 10; ++mutation)
    {
        auto invalid = request;
        switch (mutation)
        {
        case 0: invalid.magic = 0; break;
        case 1: invalid.version = 2; break;
        case 2: invalid.size = 0; break;
        case 3: invalid.processId = 43; break;
        case 4: invalid.token = 124; break;
        case 5: invalid.action = static_cast<pin::Action>(2); break;
        case 6: invalid.status = pin::Completed; break;
        case 7: invalid.path[0] = L'\0'; break;
        case 8: std::fill(std::begin(invalid.path), std::end(invalid.path), L'x'); break;
        case 9: wcscpy_s(invalid.path, L"relative.lnk"); break;
        }
        Expect(!pin::ValidRequest(invalid, 42, 123),
            "unbound, replayed, unsupported or malformed requests cannot enter Explorer pin execution");
    }
}

void TestNativeMenuThemeScope()
{
    namespace theme = snowdesktop::native_menu_theme;
    using Mode = theme::detail::PreferredAppMode;
    using snowdesktop::modern_menu::Appearance;
    struct State
    {
        Mode mode = Mode::AllowDark;
        unsigned flushes = 0;
        unsigned windowUpdates = 0;
        bool windowDark = false;
        bool highContrast = false;
    };
    static State state;
    state = {};
    const theme::detail::Api api{
        [](Mode mode) -> Mode { const auto previous = state.mode; state.mode = mode; return previous; },
        [](HWND, bool dark) -> bool { state.windowDark = dark; ++state.windowUpdates; return true; },
        [] { ++state.flushes; },
        [] { return state.highContrast; },
    };
    const HWND window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1));
    Expect(!theme::detail::SupportsPreferredAppMode(10, 17763) &&
        !theme::detail::SupportsPreferredAppMode(10, 18361) &&
        theme::detail::SupportsPreferredAppMode(10, 18362) &&
        theme::detail::SupportsPreferredAppMode(10, 26100) &&
        !theme::detail::SupportsPreferredAppMode(6, 7601),
        "native theme loading rejects the old ordinal-135 ABI and supports current Windows 10/11");

    // The native API is the only substitute. Use the real menu color resolver
    // with independent expectations, including software overrides opposite to
    // Windows, so More cannot silently revert to the OS color or stay white.
    for (const bool systemLight : {false, true})
    {
        const std::pair<Appearance, bool> cases[] = {
            {Appearance::FollowSystem, systemLight},
            {Appearance::SystemLightBlur, true}, {Appearance::SystemDarkBlur, false},
            {Appearance::OpaqueLight, true}, {Appearance::OpaqueDark, false},
            {Appearance::Win10Light, true}, {Appearance::Win10Dark, false},
        };
        for (const auto& [appearance, expectedLight] : cases)
        {
            const auto previousFlushes = state.flushes;
            {
                theme::ScopedTheme scope(
                    snowdesktop::modern_menu::appearance_rules::IsLightTheme(appearance, systemLight), api);
                scope.ApplyToWindow(window);
                Expect(state.mode == (expectedLight ? Mode::ForceLight : Mode::ForceDark) &&
                    state.windowDark == !expectedLight && state.flushes == previousFlushes + 1,
                    "native menus apply and refresh the same explicit/system colors as software menus");
            }
            Expect(state.mode == Mode::AllowDark && state.flushes == previousFlushes + 2,
                "closing a native menu restores and refreshes the previous process theme");
        }
    }
    {
        theme::ScopedTheme outer(false, api);
        {
            theme::ScopedTheme inner(true, api);
            Expect(state.mode == Mode::ForceLight, "an inner native menu can use a light override");
        }
        Expect(state.mode == Mode::ForceDark, "an inner menu restores the outer dark menu mode");
    }
    Expect(state.mode == Mode::AllowDark, "the outer menu restores the original process mode");
    state.highContrast = true;
    {
        theme::ScopedTheme scope(false, api);
        scope.ApplyToWindow(window);
        Expect(state.mode == Mode::Default && !state.windowDark,
            "high contrast preserves native accessibility colors instead of forcing dark colors");
    }
    Expect(state.mode == Mode::AllowDark, "high-contrast menus also restore the original mode");
    const auto previousFlushes = state.flushes;
    const auto previousUpdates = state.windowUpdates;
    auto unavailable = api;
    unavailable.flushMenuThemes = nullptr;
    {
        theme::ScopedTheme scope(false, unavailable);
        scope.ApplyToWindow(window);
    }
    Expect(state.mode == Mode::AllowDark && state.flushes == previousFlushes &&
        state.windowUpdates == previousUpdates,
        "missing native theme APIs leave the standard menu and process state untouched");
}

struct TemporaryDirectory
{
    std::filesystem::path path;
    TemporaryDirectory()
    {
        GUID id{};
        Expect(SUCCEEDED(CoCreateGuid(&id)), "unique temporary directory ID");
        wchar_t text[40]{};
        StringFromGUID2(id, text, 40);
        path = std::filesystem::temp_directory_path() / (std::wstring(L"SnowDesktop-New-") + text);
        Expect(std::filesystem::create_directory(path), "create isolated test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestRealNewFolderCapture()
{
    TemporaryDirectory directory;
    auto capture = std::make_shared<snowdesktop::ShellNewItemCapture>(
        directory.path.wstring(), L"desktop-files", 7);
    {
        Microsoft::WRL::ComPtr<IContextMenu> context;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(context.GetAddressOf()))), "real New handler");
        Microsoft::WRL::ComPtr<IShellExtInit> initializer;
        Expect(SUCCEEDED(context.As(&initializer)), "New handler initializer");
        PIDLIST_ABSOLUTE folder = nullptr;
        Expect(SUCCEEDED(SHParseDisplayName(directory.path.c_str(), nullptr, &folder, 0, nullptr)),
            "isolated directory PIDL");
        const auto initialized = initializer->Initialize(folder, nullptr, 0);
        CoTaskMemFree(folder);
        Expect(SUCCEEDED(initialized), "initialize isolated New menu");
        Expect(SUCCEEDED(snowdesktop::AttachShellNewItemCapture(context.Get(), capture)),
            "attach production New-item capture site");
        HMENU root = CreatePopupMenu();
        const auto queried = context->QueryContextMenu(root, 0, 1, 0x7FFF, CMF_NORMAL);
        HMENU submenu = GetSubMenu(root, 0);
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), 0);
        const UINT command = snowdesktop::FindNewFolderCommand(context.Get(), submenu);
        Expect(SUCCEEDED(queried) && command != 0, "real NewFolder command resolves");
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = CreateWindowExW(0, L"STATIC", L"New item test owner", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        invoke.lpVerb = MAKEINTRESOURCEA(command - 1);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - 1);
        const std::wstring invocationDirectory = directory.path.wstring();
        std::string ansiDirectory;
        snowdesktop::SetShellInvocationDirectory(invoke, invocationDirectory, ansiDirectory);
        invoke.nShow = SW_SHOWNORMAL;
        const HRESULT invoked = context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        DestroyWindow(invoke.hwnd);
        DestroyMenu(root);
        if (FAILED(invoked)) std::cerr << "NewFolder HRESULT: " << std::hex << invoked << std::dec << '\n';
        Expect(SUCCEEDED(invoked),
            "real Windows NewFolder command succeeds");
    }
    std::vector<std::wstring> captured;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (captured.empty() && GetTickCount64() < deadline)
    {
        capture->Consume([&](const auto& id, const auto& path, size_t index) {
            Expect(id == L"desktop-files" && index == 7, "creation keeps captured owner and insertion index");
            captured.push_back(path);
            return true;
        });
        if (!captured.empty()) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE,
            static_cast<DWORD>(deadline - std::min(deadline, GetTickCount64())), QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    Expect(captured.size() == 1 && std::filesystem::is_directory(captured.front()) &&
        std::filesystem::equivalent(std::filesystem::path(captured.front()).parent_path(), directory.path),
        "the real Shell callback reports exactly the created folder inside the isolated directory");
    Expect(std::distance(std::filesystem::directory_iterator(directory.path),
        std::filesystem::directory_iterator{}) == 1, "NewFolder creates exactly one output");
    Expect(capture->Consume([](const auto&, const auto&, size_t) { return false; }),
        "released New handler retires its completed capture");
}

void TestNativeCascadeOnPrivateDesktop()
{
    // WinRAR's deferred cascade needs the menu loop on the context menu's STA.
    // Keep the real production tracker/window/message loop; replace only the
    // extension callback with a deterministic lazy popup requiring that STA.
    struct LazyCascade
    {
        std::atomic<HWND> tracker{ nullptr };
        std::atomic<bool> cancelled{ false };
        HMENU menu = CreatePopupMenu();
        HWND window = nullptr;
        HWND dockOwner = nullptr;
        HWND observedOwner = nullptr;
        bool initializedOnOwnerThread = false;
        bool displayed = false;
        bool sourceRetained = false;
        bool forwardingOwnerRetained = false;
        bool resetDuringClose = false;
        bool lightTheme = true;
        bool themeMatches = false;
        unsigned initializationCount = 0;
        ULONGLONG displayDeadline = 0;
        ~LazyCascade()
        {
            if (dockOwner) DestroyWindow(dockOwner);
            if (window) DestroyWindow(window);
            if (menu) DestroyMenu(menu);
        }
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
        {
            auto* self = reinterpret_cast<LazyCascade*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                self = static_cast<LazyCascade*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self && message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wp) == self->menu)
            {
                // Observe the actual process mode during the production menu
                // loop. SetPreferredAppMode returns the previous mode; restore
                // it immediately without changing the menu-theme cache.
                namespace theme = snowdesktop::native_menu_theme;
                const auto& api = theme::detail::SystemApi();
                if (api.setPreferredAppMode && api.allowDarkModeForWindow && api.flushMenuThemes)
                {
                    using Mode = theme::detail::PreferredAppMode;
                    const Mode expected = api.isHighContrast() ? Mode::Default :
                        self->lightTheme ? Mode::ForceLight : Mode::ForceDark;
                    const Mode actual = api.setPreferredAppMode(expected);
                    api.setPreferredAppMode(actual);
                    self->themeMatches = actual == expected;
                }
                else
                    self->themeMatches = true; // Unsupported OS keeps the native fallback.
                ++self->initializationCount;
                DeleteMenu(self->menu, 0, MF_BYPOSITION);
                self->initializedOnOwnerThread =
                    GetWindowThreadProcessId(self->tracker.load(), nullptr) == GetCurrentThreadId();
                const HWND root = self->tracker.load();
                self->observedOwner = GetWindow(root, GW_OWNER);
                namespace dock = snowdesktop::floating_dock_rules;
                self->sourceRetained = dock::ResolvePassiveDragRevealUpdate(
                    true, false, true, false,
                    dock::IsMenuOwnedByDock(root, self->dockOwner), true, true) ==
                        dock::PassiveDragRevealAction::CancelLeave;
                self->forwardingOwnerRetained = dock::IsMenuOwnedByDock(root, self->window);
                if (self->initializedOnOwnerThread)
                {
                    AppendMenuW(self->menu, MF_STRING, 71, L"Deferred archive command");
                    AppendMenuW(self->menu, MF_STRING, 72, L"Second archive command");
                }
                return 0;
            }
            if (self && message == WM_TIMER && wp == 1)
            {
                RECT bounds{};
                self->displayed = GetMenuItemRect(self->tracker.load(), self->menu, 0, &bounds) &&
                    !IsRectEmpty(&bounds);
                // Tracker activation can pump this timer before TrackPopupMenu
                // has laid out its deferred commands. Wait for observable
                // bounds, with a deadline; a fixed first tick is not readiness.
                if (!self->displayed && GetTickCount64() < self->displayDeadline)
                    return 0;
                // Cancel on the tracker's thread, including in the negative
                // control that restores the old cross-thread implementation.
                const HWND root = self->tracker.load();
                if (self->resetDuringClose) self->tracker.store(nullptr);
                PostMessageW(root, WM_CANCELMODE, 0, 0);
                KillTimer(window, 1);
                return 0;
            }
            return DefWindowProcW(window, message, wp, lp);
        }
    } cascade;
    Expect(cascade.menu != nullptr, "create isolated deferred popup");
    AppendMenuW(cascade.menu, MF_STRING, 70, L"");
    WNDCLASSW cls{};
    cls.lpfnWndProc = LazyCascade::Proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SnowDesktopCascadeOwnerTest";
    Expect(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "register isolated cascade owner");
    cascade.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Cascade owner test",
        WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, cls.hInstance, &cascade);
    Expect(cascade.window != nullptr, "create isolated cascade owner");
    cascade.dockOwner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Source Dock",
        WS_POPUP, -32000, -32000, 1, 1, cascade.window, nullptr, cls.hInstance, nullptr);
    Expect(cascade.dockOwner != nullptr, "create a distinct source Dock owner");
    ShowWindow(cascade.dockOwner, SW_SHOWNOACTIVATE);
    UINT selected = 0;
    for (const bool lightTheme : {false, true, false})
    {
        while (GetMenuItemCount(cascade.menu) > 0)
            DeleteMenu(cascade.menu, 0, MF_BYPOSITION);
        AppendMenuW(cascade.menu, MF_STRING, 70, L"");
        cascade.displayed = false;
        cascade.lightTheme = lightTheme;
        cascade.themeMatches = false;
        cascade.displayDeadline = GetTickCount64() + 2000;
        Expect(SetTimer(cascade.window, 1, 30, nullptr) != 0, "bound the native popup lifetime");
        selected = snowdesktop::shell_popup_menu_tracker::Track(cascade.menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON, {100, 100}, cascade.window, false,
            cascade.tracker, cascade.cancelled, lightTheme, cascade.dockOwner);
        Expect(cascade.themeMatches,
            "the real native tracker applies the requested theme across dark/light/dark openings");
    }
    Expect(cascade.initializedOnOwnerThread && GetMenuItemCount(cascade.menu) == 2,
        "deferred cascade initializes on the menu-tracking STA and keeps its commands");
    Expect(cascade.displayed, "initialized deferred commands have visible native menu bounds");
    Expect(cascade.observedOwner == cascade.dockOwner && cascade.sourceRetained &&
        !cascade.forwardingOwnerRetained,
        "the real native menu retains only its source Dock while forwarding initialization to the original STA owner");
    Expect(selected == 0 && cascade.tracker.load() == nullptr,
        "cancellation invokes no command and releases the transient owner");
    Expect(!snowdesktop::floating_dock_rules::IsMenuOwnedByDock(
        cascade.tracker.load(), cascade.dockOwner),
        "the exited native menu releases its source Dock hold");

    // An unrelated native menu keeps the existing ownerless tracker behavior.
    // Simulate shutdown resetting the slot before Track returns; cleanup must
    // not restore a still-valid earlier session over that explicit reset.
    while (GetMenuItemCount(cascade.menu) > 0) DeleteMenu(cascade.menu, 0, MF_BYPOSITION);
    AppendMenuW(cascade.menu, MF_STRING, 70, L"");
    cascade.displayed = false;
    cascade.resetDuringClose = true;
    cascade.tracker.store(cascade.dockOwner);
    cascade.displayDeadline = GetTickCount64() + 2000;
    Expect(SetTimer(cascade.window, 1, 30, nullptr) != 0, "bound the ownerless native popup lifetime");
    Expect(snowdesktop::shell_popup_menu_tracker::Track(cascade.menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON, {100, 100}, cascade.window, false,
        cascade.tracker, cascade.cancelled, cascade.lightTheme) == 0 && cascade.displayed &&
        cascade.observedOwner == nullptr && !cascade.sourceRetained &&
        cascade.tracker.load() == nullptr,
        "a default native menu grants no Dock hold and an explicit session reset is not resurrected on exit");
    cascade.cancelled.store(true);
    const auto before = cascade.initializationCount;
    Expect(snowdesktop::shell_popup_menu_tracker::Track(cascade.menu,
        TPM_RETURNCMD, {100, 100}, cascade.window, false, cascade.tracker, cascade.cancelled, true) == 0 &&
        cascade.initializationCount == before && cascade.tracker.load() == nullptr,
        "early cancellation never opens or initializes the native menu");

    cascade.tracker.store(cascade.dockOwner);
    Expect(snowdesktop::shell_popup_menu_tracker::Track(cascade.menu,
        TPM_RETURNCMD, {100, 100}, cascade.window, false,
        cascade.tracker, cascade.cancelled, true, cascade.dockOwner) == 0 &&
        cascade.tracker.load() == cascade.dockOwner,
        "a cancelled inner tracker restores its still-live outer session slot");
    cascade.tracker.store(nullptr);
    HWND child = CreateWindowExW(0, L"STATIC", L"Child is not an owner", WS_CHILD,
        0, 0, 1, 1, cascade.dockOwner, nullptr, cls.hInstance, nullptr);
    HWND messageOnly = CreateWindowExW(0, L"STATIC", L"Message-only is not an owner", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    Expect(child && messageOnly, "create invalid native owner fixtures");
    cascade.cancelled.store(false);
    for (const HWND invalid : { child, messageOnly })
    {
        // If ownership validation regresses, cancel the erroneously opened
        // real menu and fail its initialization assertion instead of hanging.
        cascade.displayed = false;
        cascade.resetDuringClose = false;
        cascade.displayDeadline = GetTickCount64() + 2000;
        Expect(SetTimer(cascade.window, 1, 30, nullptr) != 0, "bound an invalid-owner regression");
        const UINT command = snowdesktop::shell_popup_menu_tracker::Track(cascade.menu,
            TPM_RETURNCMD, {100, 100}, cascade.window, false,
            cascade.tracker, cascade.cancelled, true, invalid);
        KillTimer(cascade.window, 1);
        Expect(command == 0 &&
            cascade.initializationCount == before && cascade.tracker.load() == nullptr,
            invalid == messageOnly ? "message-only HWND cannot acquire native menu ownership" :
                "child HWND cannot acquire native menu ownership");
    }
    DestroyWindow(child);
    DestroyWindow(messageOnly);

    std::promise<HWND> foreignReady;
    std::promise<void> foreignRelease;
    auto foreignReleased = foreignRelease.get_future();
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    std::thread foreign([&] {
        HWND window = nullptr;
        if (SetThreadDesktop(desktop))
            window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Other thread Dock",
                WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
        foreignReady.set_value(window);
        foreignReleased.wait();
        if (window) DestroyWindow(window);
    });
    const HWND foreignOwner = foreignReady.get_future().get();
    cascade.displayed = false;
    cascade.displayDeadline = GetTickCount64() + 2000;
    const bool boundedForeign = SetTimer(cascade.window, 1, 30, nullptr) != 0;
    const bool rejectedForeign = boundedForeign && foreignOwner && snowdesktop::shell_popup_menu_tracker::Track(
        cascade.menu, TPM_RETURNCMD, {100, 100}, cascade.window, false,
        cascade.tracker, cascade.cancelled, true, foreignOwner) == 0 &&
        cascade.initializationCount == before && cascade.tracker.load() == nullptr;
    KillTimer(cascade.window, 1);
    foreignRelease.set_value();
    foreign.join();
    Expect(rejectedForeign, "a real top-level HWND on another thread cannot join the menu's input queue");
}

thread_local unsigned preparedPopupShows = 0;
LRESULT CALLBACK ObservePreparedPopup(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0)
    {
        const auto &message = *reinterpret_cast<CWPSTRUCT *>(lp);
        if (message.message == WM_WINDOWPOSCHANGED && IsWindowVisible(message.hwnd) &&
            (reinterpret_cast<WINDOWPOS *>(message.lParam)->flags & SWP_SHOWWINDOW))
        {
            wchar_t name[32]{};
            if (GetClassNameW(message.hwnd, name, 32) && wcscmp(name, L"#32768") == 0)
                ++preparedPopupShows;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void TestPopupMaterializationOnPrivateDesktop()
{
    // Keep the production User32 loop/forwarding/cancellation. Only substitute
    // the extension callback: like a deferred IExplorerCommand adapter, it
    // provides children exclusively while its own STA has an active menu.
    struct Fixture
    {
        HMENU menu = CreatePopupMenu();
        HWND window = nullptr;
        unsigned calls = 0, commands = 0;
        bool activeMenu = false, correctPosition = false;
        bool leaveDeferred = false;
        ~Fixture()
        {
            if (window) DestroyWindow(window);
            if (menu) DestroyMenu(menu);
        }
        static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
        {
            auto *self = reinterpret_cast<Fixture *>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                self = static_cast<Fixture *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (self && message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(wp) == self->menu)
            {
                ++self->calls;
                GUITHREADINFO info{sizeof(info)};
                self->activeMenu = GetGUIThreadInfo(GetCurrentThreadId(), &info) &&
                    (info.flags & GUI_INMENUMODE) && info.hwndMenuOwner &&
                    GetWindowThreadProcessId(info.hwndMenuOwner, nullptr) == GetCurrentThreadId();
                self->correctPosition = LOWORD(lp) == 3 && HIWORD(lp) == FALSE;
                if (self->activeMenu && !self->leaveDeferred)
                {
                    DeleteMenu(self->menu, 0, MF_BYPOSITION);
                    AppendMenuW(self->menu, MF_STRING, 51, L"Add to archive");
                    AppendMenuW(self->menu, MF_STRING | MF_GRAYED, 52, L"Extract files");
                }
                return 0;
            }
            if (self && message == WM_COMMAND) ++self->commands;
            return DefWindowProcW(window, message, wp, lp);
        }
    } fixture;
    namespace ext = snowdesktop::shell_extensions;
    Expect(fixture.menu != nullptr, "create the deferred archive fixture menu");
    AppendMenuW(fixture.menu, MF_STRING, 50, L"");
    WNDCLASSW cls{};
    cls.lpfnWndProc = Fixture::Proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SnowDesktopPopupReaderFixture";
    Expect(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
        "register the deferred archive callback owner");
    fixture.window = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"", WS_POPUP,
        -32000, -32000, 1, 1, nullptr, nullptr, cls.hInstance, &fixture);
    Expect(fixture.window != nullptr, "create the callback window on the Shell STA");

    SendMessageW(fixture.window, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(fixture.menu), MAKELPARAM(3, FALSE));
    Expect(!fixture.activeMenu && ext::RequiresNativePopup(fixture.menu),
        "standalone initialization reproduces the unmaterialized archive popup");
    preparedPopupShows = 0;
    HHOOK observer = SetWindowsHookExW(WH_CALLWNDPROC, ObservePreparedPopup, nullptr, GetCurrentThreadId());
    Expect(observer != nullptr, "observe actual native popup visibility during preparation");
    const HWND foreground = GetForegroundWindow();
    const bool ready = ext::TryMaterializePopup(fixture.menu, fixture.window, 3);
    UnhookWindowsHookEx(observer);
    Expect(ready && fixture.activeMenu && fixture.correctPosition && GetMenuItemCount(fixture.menu) == 2,
        "DEFERRED_ARCHIVE_MUST_MATERIALIZE: the real same-STA menu state generates both children at the original parent position");
    Expect(preparedPopupShows == 0 && GetForegroundWindow() == foreground && fixture.commands == 0,
        "background preparation displays no native popup, changes no foreground and invokes no command");
    Expect(GetMenuState(fixture.menu, 52, MF_BYCOMMAND) & MF_GRAYED,
        "materialization retains the extension's disabled command state");
    const auto calls = fixture.calls;
    Expect(!ext::TryMaterializePopup(fixture.menu, fixture.window, 3) && fixture.calls == calls,
        "already readable cascades such as 7-Zip never initialize a second time");

    while (GetMenuItemCount(fixture.menu) > 0) DeleteMenu(fixture.menu, 0, MF_BYPOSITION);
    AppendMenuW(fixture.menu, MF_OWNERDRAW, 50, nullptr);
    Expect(!ext::TryMaterializePopup(fixture.menu, fixture.window, 3) && fixture.calls == calls &&
        ext::RequiresNativePopup(fixture.menu), "owner-drawn popups retain native rendering without being probed");
    ModifyMenuW(fixture.menu, 0, MF_BYPOSITION | MF_STRING, 50, L"...");
    fixture.leaveDeferred = true;
    Expect(!ext::TryMaterializePopup(fixture.menu, fixture.window, 3) && ext::RequiresNativePopup(fixture.menu),
        "an extension that still returns placeholders retains its native fallback");
    Expect(!ext::TryMaterializePopup(fixture.menu, nullptr, 3), "an invalid callback owner cannot prepare a popup");
}

void TestNativeCascadeOwnerThread()
{
    const std::wstring name = L"SnowDesktop.ShellTrackerTests." + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Expect(desktop != nullptr, "create a private desktop for real native tracker menus");
    std::exception_ptr failure;
    std::thread task([&] {
        try
        {
            Expect(SetThreadDesktop(desktop) != FALSE, "attach the tracker test thread to its private desktop");
            Expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
                "the private tracker thread preserves the Shell owner STA");
            try { TestPopupMaterializationOnPrivateDesktop(); TestNativeCascadeOnPrivateDesktop(); }
            catch (...) { CoUninitialize(); throw; }
            CoUninitialize();
        }
        catch (...) { failure = std::current_exception(); }
    });
    task.join();
    const bool closed = CloseDesktop(desktop) != FALSE;
    if (failure) std::rethrow_exception(failure);
    Expect(closed, "release the private desktop after the tracker thread exits");
}

void RunTests()
{
    TestStartPinRouting();
    TestNativeMenuThemeScope();
    TestNativeCascadeOwnerThread();
    const std::wstring currentDirectory =
        std::filesystem::current_path().wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(
            currentDirectory) == currentDirectory,
        "a directory item invokes Shell commands in that directory");

    const std::wstring filePath =
        (std::filesystem::current_path() /
            L"snowdesktop-shell-invoke-probe.txt").wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(filePath) ==
            currentDirectory,
        "a file item invokes Shell commands in its parent directory");

    const std::wstring desktopDirectory =
        snowdesktop::DesktopShellInvocationDirectory();
    Expect(!desktopDirectory.empty() &&
            std::filesystem::is_directory(desktopDirectory),
        "the physical desktop directory is available for background verbs");

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    std::string ansiDirectory;
    snowdesktop::SetShellInvocationDirectory(
        invoke, currentDirectory, ansiDirectory);
    Expect(invoke.lpDirectoryW == currentDirectory.c_str(),
        "the Unicode Shell invocation directory uses stable caller storage");
    Expect(invoke.lpDirectory != nullptr &&
            !ansiDirectory.empty(),
        "legacy Shell handlers also receive an ANSI invocation directory");

    // Query the real Windows New handler without displaying a menu or creating
    // files. This catches unsupported canonical verbs and lazy submenu loading.
    {
        Microsoft::WRL::ComPtr<IContextMenu> context;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(context.GetAddressOf()))),
            "Windows New-menu handler is available");
        Microsoft::WRL::ComPtr<IShellExtInit> initializer;
        Expect(SUCCEEDED(context.As(&initializer)), "New handler accepts a folder");
        PIDLIST_ABSOLUTE folder = nullptr;
        Expect(SUCCEEDED(SHParseDisplayName(currentDirectory.c_str(), nullptr,
                &folder, 0, nullptr)), "test folder resolves to a PIDL");
        const HRESULT initialized = initializer->Initialize(folder, nullptr, 0);
        CoTaskMemFree(folder);
        Expect(SUCCEEDED(initialized), "New handler initializes for the target folder");
        HMENU root = CreatePopupMenu();
        Expect(SUCCEEDED(context->QueryContextMenu(root, 0, 1, 0x7FFF, CMF_NORMAL)),
            "New handler builds its commands");
        HMENU submenu = GetSubMenu(root, 0);
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), 0);
        const UINT command = snowdesktop::FindNewFolderCommand(context.Get(), submenu);
        Expect(command != 0, "Ctrl+Shift+N resolves the real NewFolder command");
        EnableMenuItem(submenu, command, MF_BYCOMMAND | MF_GRAYED);
        Expect(snowdesktop::FindNewFolderCommand(context.Get(), submenu) == 0,
            "a disabled NewFolder command cannot be executed");
        DestroyMenu(root);
    }
    TestRealNewFolderCapture();
}
} // namespace

// Own uniquely named HKCU file-type keys only; never modify real handlers.
struct TemporaryVerb
{
    std::wstring extension, progId;
    HKEY verb = nullptr;
    std::vector<std::wstring> owned;
    void Clear() noexcept
    {
        if (verb) { RegCloseKey(verb); verb=nullptr; }
        for (const auto &path : owned) RegDeleteTreeW(HKEY_CURRENT_USER,path.c_str());
        if (!owned.empty()) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        owned.clear();
    }
    static void Value(HKEY key, const wchar_t *name, const std::wstring &text)
    {
        Expect(RegSetValueExW(key,name,0,REG_SZ,reinterpret_cast<const BYTE*>(text.c_str()),
            static_cast<DWORD>((text.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"write owned test metadata");
    }
    TemporaryVerb()
    {
        GUID guid{}; Expect(SUCCEEDED(CoCreateGuid(&guid)),"unique association ID");
        wchar_t id[40]{}; StringFromGUID2(guid,id,40);
        extension=std::wstring(L".snowmenu-")+id; progId=std::wstring(L"SnowDesktop.MenuTest.")+id;
        try
        {
            for (const auto &name : {extension,progId})
            {
                const auto path=L"Software\\Classes\\"+name;
                HKEY key=nullptr; DWORD disposition=0;
                Expect(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,
                    &key,&disposition)==ERROR_SUCCESS,"create private association");
                RegCloseKey(key);
                Expect(disposition==REG_CREATED_NEW_KEY,"never replace an existing association");
                owned.push_back(path);
            }
            HKEY key=nullptr;
            Expect(RegOpenKeyExW(HKEY_CURRENT_USER,owned[0].c_str(),0,KEY_SET_VALUE,&key)==ERROR_SUCCESS,"open owned extension");
            const auto status=RegSetValueExW(key,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(progId.c_str()),
                static_cast<DWORD>((progId.size()+1)*sizeof(wchar_t)));
            RegCloseKey(key); Expect(status==ERROR_SUCCESS,"associate test file");
            const auto path=owned[1]+L"\\shell\\SnowDesktopProbe";
            Expect(RegCreateKeyExW(HKEY_CURRENT_USER,path.c_str(),0,nullptr,0,KEY_READ|KEY_WRITE,nullptr,
                &verb,nullptr)==ERROR_SUCCESS,"create private verb");
            Value(verb,nullptr,L"SnowDesktop isolated policy probe");
            HKEY command=nullptr;
            Expect(RegCreateKeyExW(verb,L"command",0,nullptr,0,KEY_SET_VALUE,nullptr,&command,nullptr)==ERROR_SUCCESS,
                "create private command metadata");
            constexpr wchar_t executable[]=L"notepad.exe \"%1\""; // Never invoked.
            const auto written=RegSetValueExW(command,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(executable),sizeof(executable));
            RegCloseKey(command); Expect(written==ERROR_SUCCESS,"write private command metadata");
            SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        }
        catch (...) { Clear(); throw; }
    }
    ~TemporaryVerb() { Clear(); }
    void Disable()
    {
        Value(verb,L"LegacyDisable",L"");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
};
template<class Wait>
void TestSystemPolicy(Wait wait, const std::filesystem::path &directory)
{
    namespace ext=snowdesktop::shell_extensions;
    TemporaryVerb registration;
    const auto file=directory/(L"sample"+registration.extension);
    { std::ofstream output(file); output<<"isolated Shell policy sample"; }
    ext::Request request; request.paths={file.wstring()};
    const auto probe=[](const auto &list) {
        return std::find_if(list.begin(),list.end(),[](const auto &e){return e.label==L"SnowDesktop isolated policy probe";});
    };
    {
        ext::Session visible(request); const auto reply=wait(visible);
        if (!reply.ok || probe(reply.entries) == reply.entries.end())
            std::cerr << "Private policy query: ok=" << reply.ok << ", error=" << reply.error
                      << ", entries=" << reply.entries.size() << std::endl;
        Expect(reply.ok&&probe(reply.entries)!=reply.entries.end(),
            "system-enabled private verb appears through the actual Shell aggregate");
    }
    registration.Disable();
    {
        ext::Session disabled(request); const auto reply=wait(disabled);
        const auto shown=ext::VisibleEntries({},reply.entries,request);
        Expect(reply.ok&&probe(shown)==shown.end(),
            "following the Shell never restores a system-disabled command");
    }
}
// Real inherited pipes and process supervision; only the third-party query is
// substituted, so a hung extension cannot be mistaken for a passing UI mock.
void TestDeferredPopups()
{
    // Actual HMENU dummy rows reproduce the Shell placeholder shape, including
    // a command ID which the old reader incorrectly treated as executable.
    using snowdesktop::shell_extensions::RequiresNativePopup;
    HMENU menu = CreatePopupMenu();
    Expect(menu != nullptr, "create isolated native submenu");
    struct Cleanup
    {
        HMENU menu;
        ~Cleanup()
        {
            DestroyMenu(menu);
        }
    } cleanup{menu};
    AppendMenuW(menu, MF_STRING, 41, L"");
    Expect(RequiresNativePopup(menu),
           "an unnamed dummy command defers the parent popup, not an ellipsis child");
    ModifyMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 41, L"...");
    Expect(RequiresNativePopup(menu), "a lazy ellipsis row opens the native parent directly");
    ModifyMenuW(menu, 0, MF_BYPOSITION | MF_STRING, 41, L"Open PowerShell here");
    Expect(!RequiresNativePopup(menu), "materialized submenu commands retain custom rendering");
    AppendMenuW(menu, MF_OWNERDRAW, 42, nullptr);
    Expect(RequiresNativePopup(menu), "unlabelled owner-drawn children keep their native parent renderer");

    HMENU root = CreatePopupMenu();
    Expect(root != nullptr, "create aggregate command-scope fixture");
    Cleanup rootCleanup{root};
    AppendMenuW(root, MF_STRING, 40, L"Run selected application as administrator");
    AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(menu), L"Provider tools");
    Expect(snowdesktop::shell_extensions::IsRootMenuCommand(root, 40) &&
        !snowdesktop::shell_extensions::IsRootMenuCommand(root, 41),
        "selected-object root operations remain host-owned while provider children keep their own scope");
    // Detach before the two independently owned fixtures destroy their menus.
    RemoveMenu(root, 1, MF_BYPOSITION);
}

// Opt-in installed-extension evidence through the full supervised Host/Session
// path. This reads only isolated files and never invokes archive commands.
void ProbeArchiveSubmenus()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    for (const bool archive : {false, true})
    {
        const auto path = directory.path / (archive ? L"Sample.zip" : L"Sample.txt");
        {
            std::ofstream file(path, std::ios::binary);
            if (archive)
            {
                const char emptyZip[22] = {'P', 'K', 5, 6};
                file.write(emptyZip, sizeof(emptyZip));
            }
            else file << "isolated archive submenu probe";
        }
        ext::Request request; request.paths = {path.wstring()};
        ext::Session session(request);
        std::optional<ext::Reply> reply;
        const auto deadline = GetTickCount64() + 12000;
        while (!reply && GetTickCount64() < deadline)
        {
            reply = session.Poll();
            if (reply) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (reply && !reply->ok) std::cout << "Archive query failed: " << reply->error << std::endl;
        Expect(reply && reply->ok, "the real archive query completes through supervised Session IPC");
        bool winrar = false;
        for (const auto &entry : reply->entries)
        {
            if (entry.label != L"WinRAR" && entry.label != L"7-Zip") continue;
            std::cout << (archive ? "ZIP " : "TXT ") << (entry.label == L"WinRAR" ? "WinRAR" : "7-Zip")
                << ": native=" << entry.native << ", children=" << entry.children.size() << std::endl;
            Expect(!entry.native && !entry.children.empty(), "installed archive handlers return a real custom submenu tree");
            unsigned commands = 0;
            for (const auto &child : entry.children)
            {
                if (child.separator) continue;
                Expect(!child.label.empty() && child.label != L"…" && child.label != L"...",
                    "the archive submenu contains no dummy or ellipsis commands");
                if (child.enabled && child.children.empty())
                {
                    Expect(ext::ResolveCommand(*reply, ext::AppendReference(ext::AppendReference({}, entry), child)) == child.token && child.token,
                        "each archive command resolves to its fresh live-session token");
                    ++commands;
                }
            }
            Expect(commands > 0, "the real archive submenu exposes executable children");
            winrar |= entry.label == L"WinRAR";
        }
        Expect(winrar, "this opt-in probe requires an enabled installed WinRAR Shell extension");
    }
}

// Unlike metadata-only archive probes, this opt-in exercises a real WinRAR
// compression of private files and checks the archive's independently listed
// contents. It never uses a real user's shortcut or target.
void ProbeShortcutArchives()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    std::filesystem::create_directory(directory.path / L"target");
    const auto target = directory.path / L"target" / L"RealTarget.txt";
    std::ofstream(target) << "referenced target must stay outside the archive";
    const auto shortcut = directory.path / L"ActualShortcut.lnk";
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    Expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&persist)) && SUCCEEDED(link->SetPath(target.c_str())) &&
        SUCCEEDED(persist->Save(shortcut.c_str(), TRUE)), "create an isolated shortcut with a distinct target");
    link.Reset(); persist.Reset();
    ext::Request request; request.paths = {shortcut.wstring()};
    ext::MenuSnapshotCache cache(directory.path / L"cache");
    const auto ticket = cache.Capture(request);
    ext::Session session(request);
    std::optional<ext::Reply> reply;
    const auto deadline = GetTickCount64() + 12000;
    while (!reply && GetTickCount64() < deadline)
    {
        reply = session.Poll();
        if (reply) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (reply && !reply->ok) std::cout << reply->error << std::endl;
    Expect(reply && reply->ok, "shortcut query completes through the production helper");
    const auto root = std::find_if(reply->entries.begin(), reply->entries.end(), [](const auto &entry) {
        return entry.label == L"WinRAR";
    });
    Expect(root != reply->entries.end() && !root->native, "enabled installed WinRAR exposes a real shortcut submenu");
    const auto compress = std::find_if(root->children.begin(), root->children.end(), [](const auto &entry) {
        // The aggregate may expose this provider through IExplorerCommand,
        // whose child lacks a verb. Its actual label and live token identify
        // the installed command; the referenced target's name must fail here.
        return entry.label == L"Add to \"ActualShortcut.rar\"";
    });
    Expect(compress != root->children.end(), "WinRAR compression names the selected shortcut, never RealTarget.rar");
    Expect(cache.Store(ticket, *reply) && cache.Find(cache.Capture(request)).has_value(),
        "the original shortcut keeps a reusable display snapshot");
    const auto token = ext::ResolveCommand(*reply, ext::AppendReference(ext::AppendReference({}, *root), *compress));
    Expect(token != 0, "actual shortcut command resolves against its live session");
    session.Invoke(token, {});
    const auto archive = directory.path / L"ActualShortcut.rar";
    const auto archiveDeadline = GetTickCount64() + 15000;
    bool ready = false;
    while (!ready && GetTickCount64() < archiveDeadline)
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow)
        {
            HANDLE file = CreateFileW(archive.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { ready = true; CloseHandle(file); }
        }
        if (ready) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    Expect(ready && !std::filesystem::exists(target.parent_path() / L"RealTarget.rar"),
        "WinRAR creates the archive beside the shortcut instead of its referenced target");
    wchar_t module[32768]{}; DWORD bytes = sizeof(module);
    Expect(RegGetValueW(HKEY_CLASSES_ROOT,
        L"CLSID\\{B41DB860-64E4-11D2-9906-E49FADC173CA}\\InProcServer32", nullptr,
        RRF_RT_REG_SZ, nullptr, module, &bytes) == ERROR_SUCCESS, "read the registered installed WinRAR module");
    const auto rar = std::filesystem::path(module).parent_path() / L"Rar.exe";
    Expect(std::filesystem::is_regular_file(rar), "installed Rar CLI is available for independent content validation");
    const auto listing = directory.path / L"archive-list.txt";
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output = CreateFileW(listing.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE, "create isolated CLI streams");
    std::wstring command = L"\"" + rar.wstring() + L"\" lb \"" + archive.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input; startup.hStdOutput = startup.hStdError = output;
    PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(rar.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, directory.path.c_str(), &startup, &process) != FALSE;
    CloseHandle(input); CloseHandle(output);
    Expect(launched, "list only the private archive through installed Rar CLI");
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    DWORD exit = 1; GetExitCodeProcess(process.hProcess, &exit);
    if (waited != WAIT_OBJECT_0)
    {
        // This is our own bounded CLI child, never a user's WinRAR process.
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 1000);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    Expect(waited == WAIT_OBJECT_0 && exit == 0, "independent Rar content listing completes successfully");
    std::ifstream contents(listing, std::ios::binary);
    const std::string names{std::istreambuf_iterator<char>(contents), std::istreambuf_iterator<char>()};
    Expect(names.find("ActualShortcut.lnk") != std::string::npos && names.find("RealTarget.txt") == std::string::npos,
        "the actual archive contains the shortcut file and excludes its referenced target");
    std::cout << "WinRAR shortcut archive contents: " << names << std::endl;
    Expect(std::filesystem::exists(shortcut) && std::filesystem::exists(target), "compression preserves both original files");

    // Target delegation also affects folder links, broken links and mixed
    // selections. Query these real objects without invoking another archive.
    auto makeLink = [&](const std::filesystem::path &path, const std::filesystem::path &destination) {
        Microsoft::WRL::ComPtr<IShellLinkW> value;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&value))) &&
            SUCCEEDED(value.As(&file)) && SUCCEEDED(value->SetPath(destination.c_str())) &&
            SUCCEEDED(file->Save(path.c_str(), TRUE)), "create a private shortcut selection variant");
    };
    const auto folderLink = directory.path / L"FolderShortcut.lnk";
    const auto brokenLink = directory.path / L"BrokenShortcut.lnk";
    const auto otherLink = target.parent_path() / L"OtherShortcut.lnk";
    makeLink(folderLink, target.parent_path());
    makeLink(brokenLink, target.parent_path() / L"MissingTarget.txt");
    makeLink(otherLink, target);
    for (const auto &paths : std::vector<std::vector<std::wstring>>{
        {folderLink.wstring()}, {brokenLink.wstring()}, {shortcut.wstring(), otherLink.wstring(), archive.wstring()}})
    {
        std::cout << "Query shortcut variant: " << std::filesystem::path(paths.front()).filename().string()
            << ", objects=" << paths.size() << std::endl;
        ext::Request variant; variant.paths = paths;
        ext::Session query(variant);
        std::optional<ext::Reply> result;
        const auto end = GetTickCount64() + 12000;
        while (!result && GetTickCount64() < end)
        {
            result = query.Poll();
            if (result) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (result && !result->ok) std::cout << result->error << std::endl;
        Expect(result && result->ok, "real folder, broken and cross-folder shortcut queries succeed");
        const auto winrar = std::find_if(result->entries.begin(), result->entries.end(), [](const auto &entry) {
            return entry.label == L"WinRAR";
        });
        Expect(winrar != result->entries.end() && !winrar->native && !winrar->children.empty(),
            "shortcut variants preserve a real installed WinRAR submenu");
        for (const auto &child : winrar->children)
            Expect(child.label.find(L"RealTarget") == std::wstring::npos &&
                child.label.find(L"MissingTarget") == std::wstring::npos,
                "WinRAR variant menus never describe an unselected referenced file");
        std::cout << "WinRAR original shortcut selection: " << paths.size() << " object(s), children="
            << winrar->children.size() << std::endl;
    }
}

// Read installed PowerShell cascades without launching a terminal or UAC.
void ProbePowerShellSubmenus()
{
    namespace ext = snowdesktop::shell_extensions;
    for (const auto context : {ext::Context::Desktop, ext::Context::FolderBackground})
    {
        ext::Request request; request.catalogueOnly = true; request.context = context;
        ext::Session session(request);
        std::optional<ext::Reply> reply;
        const auto deadline = GetTickCount64() + 12000;
        while (!reply && GetTickCount64() < deadline)
        {
            reply = session.Poll();
            if (reply) break;
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        Expect(reply && reply->ok, "the real background menu query completes through Session IPC");
        const auto root = std::find_if(reply->entries.begin(), reply->entries.end(), [](const auto &entry) {
            auto key = entry.key;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
            return key == "powershell7x64" || key == "powershell7x86";
        });
        Expect(root != reply->entries.end(), "this opt-in probe requires enabled installed PowerShell 7 menu registration");
        std::cout << (context == ext::Context::Desktop ? "Desktop" : "Folder background")
            << " PowerShell 7: native=" << root->native << ", children=" << root->children.size() << std::endl;
        Expect(!root->native && !root->children.empty(), "PowerShell 7 is a real custom submenu");
        bool ordinary = false, administrator = false;
        for (const auto &child : root->children)
        {
            ordinary |= child.key == "openpwsh";
            administrator |= child.key == "runas";
            if (child.separator) continue;
            Expect(child.token && ext::ResolveCommand(*reply,
                ext::AppendReference(ext::AppendReference({}, *root), child)) == child.token,
                "each PowerShell child resolves to its current live-session command");
        }
        Expect(ordinary && administrator, "PowerShell preserves both ordinary and administrator commands");
        ext::Preferences visible;
        ext::SetHidden(visible, root->provider, context, false);
        const auto shown = ext::VisibleSnapshot(visible, *reply, ext::ContextBit(context));
        Expect(shown.size() == 1 && shown.front().children.size() == root->children.size(),
            "the exposed background visibility path preserves the complete enabled PowerShell cascade");
    }
}

void TestRegistryCatalogue()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto path = L"Software\\SnowDesktopCatalogueTests\\" + temp.path.filename().wstring();
    struct RegistryFixture
    {
        HKEY key = nullptr; std::wstring path;
        ~RegistryFixture() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } registry{nullptr, path};
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &registry.key, nullptr) == ERROR_SUCCESS, "private registry fixture");
    auto put = [&](const wchar_t *key, const wchar_t *name, const wchar_t *value) {
        HKEY created = nullptr;
        Expect(RegCreateKeyExW(registry.key, key, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &created, nullptr) == ERROR_SUCCESS, "create fixture registration");
        const auto status = RegSetValueExW(created, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(created); Expect(status == ERROR_SUCCESS, "write fixture registration");
    };
    put(L".snowtest", nullptr, L"SnowTest.Document");
    put(L"SnowTest.Document\\shell\\inspect", L"MUIVerb", L"Same name");
    put(L"SnowTest.Document\\shell\\inspect\\command", nullptr, L"unused.exe %1");
    put(L"*\\shell\\other", L"MUIVerb", L"Same name");
    put(L"*\\shell\\hidden", L"LegacyDisable", L"");
    put(L"*\\shellex\\ContextMenuHandlers\\one", nullptr, L"{B92A9760-188A-44ED-88A5-F9E3D30E33AF}");
    put(L"Directory\\shellex\\ContextMenuHandlers\\two", nullptr, L"{B92A9760-188A-44ED-88A5-F9E3D30E33AF}");
    auto catalogue = ext::ReadCatalogue(registry.key, false);
    Expect(catalogue.rows.size() == 4, "registry catalogue finds type-specific verbs and merges a handler by CLSID");
    auto find = [&](const std::string &id) -> ext::Registration& {
        auto row = std::find_if(catalogue.rows.begin(), catalogue.rows.end(), [&](const auto &r) { return r.id == id; });
        Expect(row != catalogue.rows.end(), "expected registration exists"); return *row;
    };
    auto &handler = find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}");
    Expect(handler.sources.size() == 2 && handler.contexts == 3 && !handler.linked, "one handler preserves both scopes without claiming a runtime association");
    Expect(!find("reg:*\\shell\\hidden").systemEnabled, "system-disabled registration is excluded from available settings");
    ext::Request request; request.paths = {L"C:\\fixture.snowtest"}; request.context = ext::Context::File;
    ext::Reply reply; reply.ok = true;
    ext::Entry actual; actual.key = "inspect"; actual.provider = "verb:inspect"; actual.label = L"Same name";
    reply.entries = {actual}; ext::Associate(catalogue, request, reply);
    Expect(reply.entries[0].registration == "reg:snowtest.document\\shell\\inspect", "unique canonical verb associates the actual type-specific command");
    reply.entries[0].key.clear(); reply.entries[0].registration.clear(); ext::Associate(catalogue, request, reply);
    Expect(reply.entries[0].registration.empty(), "a matching display name never establishes ownership");
    JsonValue legacy;
    Expect(ParseJson(R"({"shown":[{"id":"verb:inspect","context":0},{"id":"both","context":0},{"id":"both","context":1}]})", legacy), "legacy fixture parses");
    auto prefs = ext::ReadPreferences(&legacy);
    Expect(prefs.rulesVersion == 2 && !ext::IsHidden(prefs, "verb:inspect", ext::Context::File) && ext::IsHidden(prefs, "verb:inspect", ext::Context::Folder), "migration preserves different old scopes without broadening visibility");
    Expect(ext::CommonShown(prefs, "both", ext::Category::Objects), "matching old states merge into common rule");
    ext::MigrateAssociations(prefs, catalogue);
    Expect(!ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File) && ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::Folder), "identity migration preserves the exact old enabled scope");
    ext::SetOverride(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File, ext::Visibility::Inherit);
    ext::MigrateAssociations(prefs, catalogue);
    Expect(ext::OverrideOf(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File) == ext::Visibility::Inherit && ext::IsHidden(prefs, "reg:snowtest.document\\shell\\inspect", ext::Context::File), "restored inheritance does not resurrect a retained legacy opt-in");
    ext::SetCommon(prefs, "both", ext::Category::Objects, true);
    ext::SetOverride(prefs, "both", ext::Context::Folder, ext::Visibility::Hide);
    Expect(ext::IsHidden(prefs, "both", ext::Context::Folder), "location hiding takes precedence over common visibility");
    ext::SetOverride(prefs, "both", ext::Context::Folder, ext::Visibility::Inherit);
    Expect(!ext::IsHidden(prefs, "both", ext::Context::Folder), "restore inheritance removes the exception");
    JsonValue saved; Expect(ParseJson(ext::WritePreferences(prefs), saved) && ext::ReadPreferences(&saved) == prefs, "rules and retained legacy records round trip");
    // Same action registered per image type must produce one management row,
    // while distinct executables/arguments and unresolved captions stay apart.
    for (const auto *type : {L".png", L".jpg", L".jpeg"})
    {
        const auto root = std::wstring(type) + L"\\shell\\wallpaper";
        put(root.c_str(), L"MUIVerb", L"Set wallpaper");
        put((root + L"\\command").c_str(), nullptr, L"\"C:\\PhotoTool.exe\" /wallpaper \"%1\"");
    }
    put(L".bmp\\shell\\wallpaper", L"MUIVerb", L"Set wallpaper");
    put(L".bmp\\shell\\wallpaper\\command", nullptr, L"\"C:\\OtherTool.exe\" /wallpaper \"%1\"");
    put(L".gif\\shell\\wallpaper", L"MUIVerb", L"Set wallpaper");
    put(L".gif\\shell\\wallpaper\\command", nullptr, L"\"C:\\PhotoTool.exe\" /preview \"%1\"");
    put(L".tif\\shell\\wallpaper", L"MUIVerb", L"Set wallpaper");
    auto typed = ext::ReadCatalogue(registry.key, false);
    std::erase_if(typed.rows, [](const auto &r) { return r.id.find("wallpaper") == std::string::npos; });
    for (auto &row : typed.rows) row.linked = true;
    auto grouped = ext::ManagementRows(typed, ext::Category::Objects);
    Expect(grouped.size() == 4, "equivalent image commands merge but different executable, arguments and unresolved commands do not");
    const auto merged = std::find_if(grouped.begin(), grouped.end(), [](const auto &r) { return r.members.size() == 3; });
    Expect(merged != grouped.end() && merged->types == std::vector<std::wstring>{L".jpeg", L".jpg", L".png"},
        "one switch retains all three file-type identities and displays unique extensions");
    ext::Preferences original;
    ext::SetCommon(original, merged->members.front().id, ext::Category::Objects, true);
    const auto savedOriginal = original;
    Expect(!ext::ManagementCommon(original, *merged, ext::Category::Objects).has_value() && original == savedOriginal,
        "mixed existing rules are reported without rewriting or broadening them");
    ext::SetManagementCommon(original, *merged, ext::Category::Objects, true);
    for (const auto &member : merged->members)
    {
        ext::Reply menu; menu.ok = true; ext::Entry item; item.provider = "wallpaper"; item.registration = member.id; menu.entries = {item};
        Expect(ext::VisibleSnapshot(original, menu, 1).size() == 1, "one grouped toggle enables every original registration in the actual visibility path");
    }
    ext::SetManagementOverride(original, *merged, ext::Context::File, ext::Visibility::Hide);
    for (const auto &member : merged->members) Expect(ext::IsHidden(original, member.id, ext::Context::File), "grouped location exception controls all applicable type members");
    ext::SetManagementCommon(original, *merged, ext::Category::Objects, true);
    for (const auto &member : merged->members) Expect(!ext::IsHidden(original, member.id, ext::Context::File), "primary show switch overrides an earlier location hide");
    ext::SetManagementOverride(original, *merged, ext::Context::File, ext::Visibility::Show);
    ext::SetOverride(original, merged->members.front().id, ext::Context::Desktop, ext::Visibility::Show);
    ext::SetManagementCommon(original, *merged, ext::Category::Objects, false);
    for (const auto &member : merged->members) Expect(ext::IsHidden(original, member.id, ext::Context::File), "primary hide switch overrides retained migrated opt-ins");
    Expect(!ext::IsHidden(original, merged->members.front().id, ext::Context::Desktop), "object switch leaves background exceptions intact");
    Expect(ext::ManagementRows(typed, ext::Category::Objects, L".JPG").size() == 1, "type search retains the complete merged action");
    typed.rows.front().systemEnabled = false;
    grouped = ext::ManagementRows(typed, ext::Category::Objects);
    Expect(std::none_of(grouped.begin(), grouped.end(), [&](const auto &r) { return std::any_of(r.members.begin(), r.members.end(), [&](const auto &m) { return m.id == typed.rows.front().id; }); }),
        "system-disabled registrations cannot be enabled by a grouped switch");

    // Private, non-executable fixture files prove attribution comes from the
    // registered module rather than captions. No fixture command is invoked.
    const auto program = temp.path / L"Provider Tool.exe", module = temp.path / L"Provider Shell.dll";
    std::ofstream(program) << "metadata fixture"; std::ofstream(module) << "metadata fixture";
    const auto command = L"\"" + program.wstring() + L"\" \"%1\"";
    put(L"*\\shell\\provided", L"MUIVerb", L"Unrelated caption");
    put(L"*\\shell\\provided\\command", nullptr, command.c_str());
    put(L"*\\shell\\second\\command", nullptr, (command + L" /other-action").c_str());
    put(L"*\\shell\\unquoted\\command", nullptr, (program.wstring() + L" \"%1\"").c_str());
    put(L"*\\shell\\unknown", L"MUIVerb", L"Provider Tool.exe");
    put(L"*\\shell\\script\\command", nullptr, (L"cmd.exe /c " + command).c_str());
    put(L"*\\shell\\library\\command", nullptr, (L"rundll32.exe \"" + module.wstring() + L"\",Entry %1").c_str());
    put(L"CLSID\\{B92A9760-188A-44ED-88A5-F9E3D30E33AF}\\InprocServer32", nullptr, module.c_str());
    catalogue = ext::ReadCatalogue(registry.key, false);
    const auto app = find("reg:*\\shell\\provided").application;
    Expect(app.name == L"Provider Tool.exe" && !app.id.empty(), "application metadata comes from a quoted registered executable, not the menu caption");
    Expect(find("reg:*\\shell\\second").application == app, "different commands from the same executable share an application filter");
    Expect(find("reg:*\\shell\\unquoted").application == app, "installer commands with unquoted executable spaces retain module attribution");
    Expect(find("reg:*\\shell\\unknown").application.id.empty() && find("reg:*\\shell\\script").application.id.empty(),
        "unknown captions and interpreter payloads are not guessed as an application");
    Expect(find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}").application.name == L"Provider Shell.dll" &&
        find("reg:*\\shell\\library").application.name == L"Provider Shell.dll", "CLSID and rundll32 registrations identify the providing DLL rather than its generic host");

    std::filesystem::remove(module);
    catalogue = ext::ReadCatalogue(registry.key, false);
    Expect(find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}").application.id.empty() &&
        find("reg:*\\shell\\library").application.id.empty(), "a fresh catalogue drops attribution when the registered module has been removed");
    std::ofstream(module) << "metadata fixture restored";
    put(L"CLSID\\{B92A9760-188A-44ED-88A5-F9E3D30E33AF}\\InprocServer32", nullptr, program.c_str());
    put(L"CLSID\\{B92A9760-188A-44ED-88A5-F9E3D30E33AF}", nullptr, L"Updated provider");
    catalogue = ext::ReadCatalogue(registry.key, false);
    Expect(find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}").application == app &&
        find("clsid:{b92a9760-188a-44ed-88a5-f9e3d30e33af}").display.label == L"Updated provider",
        "a fresh catalogue observes changed provider registration and label");
    Expect(find("reg:*\\shell\\library").application.name == L"Provider Shell.dll",
        "a fresh catalogue rechecks previously missing command modules after their restoration");
}

void TestNvidiaCompatibility()
{
    namespace ext = snowdesktop::shell_extensions;
    // System registration and class-factory failures are the external boundary.
    // The real eligibility, native default-command selection, policy reader,
    // catalogue association and visibility paths remain under test.
    ext::Request desktop; desktop.context = ext::Context::Desktop; desktop.background = true;
    Expect(ext::NvidiaCompatibilityRequired(desktop, true, true, CLASS_E_CLASSNOTAVAILABLE),
        "a registered enabled NVIDIA extension rejected by the process gate gets compatibility");
    for (const HRESULT status : {S_OK, E_ACCESSDENIED, REGDB_E_CLASSNOTREG, E_FAIL})
        Expect(!ext::NvidiaCompatibilityRequired(desktop, true, true, status),
            "a working, denied, missing or unrelated failed provider never gets a duplicate or policy bypass");
    Expect(!ext::NvidiaCompatibilityRequired(desktop, false, true, CLASS_E_CLASSNOTAVAILABLE) &&
        !ext::NvidiaCompatibilityRequired(desktop, true, false, CLASS_E_CLASSNOTAVAILABLE),
        "removed and system-blocked NVIDIA registrations stay absent");
    for (auto context : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground})
    {
        auto request = desktop; request.context = context;
        Expect(!ext::NvidiaCompatibilityRequired(request, true, true, CLASS_E_CLASSNOTAVAILABLE),
            "NVIDIA compatibility does not add application launchers to object or folder menus");
    }
    auto probe = desktop; probe.sourceClsid = ext::NvidiaControlPanelClsid;
    Expect(!ext::NvidiaCompatibilityRequired(probe, true, true, CLASS_E_CLASSNOTAVAILABLE),
        "metadata attribution probes never contribute executable compatibility entries");

    TemporaryDirectory temp;
    struct Registry
    {
        std::wstring path; HKEY key = nullptr;
        ~Registry() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } registry{L"Software\\SnowDesktopNvidiaTests\\" + temp.path.filename().wstring()};
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, registry.path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &registry.key, nullptr) == ERROR_SUCCESS, "private NVIDIA registration fixture");
    auto put = [&](const wchar_t *path, const wchar_t *name, const wchar_t *value) {
        HKEY key = nullptr;
        Expect(RegCreateKeyExW(registry.key, path, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS,
            "create private NVIDIA metadata");
        const auto status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(key); Expect(status == ERROR_SUCCESS, "write private NVIDIA metadata");
    };
    constexpr auto handler = L"Directory\\Background\\shellex\\ContextMenuHandlers\\AliasName";
    Expect(!ext::NvidiaControlPanelRegistered(registry.key), "no registration is not an available menu");
    put(handler, nullptr, L"{F2E8B4A1-9C7D-4F6E-B3A5-8D2C1F4E9B7A}");
    Expect(!ext::NvidiaControlPanelRegistered(registry.key), "the separate NVIDIA App cannot authorize a Control Panel entry");
    put(handler, nullptr, ext::NvidiaControlPanelClsid);
    Expect(ext::NvidiaControlPanelRegistered(registry.key), "registration identity is the CLSID, not its installer-chosen key name");
    Expect(ext::HandlerEnabled(ext::NvidiaControlPanelClsid, registry.key, registry.key), "unblocked handler is enabled");
    put(L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked", ext::NvidiaControlPanelClsid, L"");
    Expect(!ext::HandlerEnabled(ext::NvidiaControlPanelClsid, registry.key, registry.key), "Blocked policy also denies compatibility");
    RegDeleteTreeW(registry.key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Blocked");
    HKEY policy = nullptr;
    Expect(RegCreateKeyExW(registry.key, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", 0,
        nullptr, 0, KEY_ALL_ACCESS, nullptr, &policy, nullptr) == ERROR_SUCCESS, "private approval policy");
    const DWORD enforce = 1;
    const auto policyStatus = RegSetValueExW(policy, L"EnforceShellExtensionSecurity", 0, REG_DWORD,
        reinterpret_cast<const BYTE *>(&enforce), sizeof(enforce));
    RegCloseKey(policy);
    Expect(policyStatus == ERROR_SUCCESS && !ext::HandlerEnabled(ext::NvidiaControlPanelClsid, registry.key, registry.key),
        "enforced approval cannot be bypassed by compatibility");
    put(L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved", ext::NvidiaControlPanelClsid, L"NVIDIA");
    Expect(ext::HandlerEnabled(ext::NvidiaControlPanelClsid, registry.key, registry.key), "approved registered handler is eligible");

    auto catalogue = ext::ReadCatalogue(registry.key, false);
    ext::Reply reply; reply.ok = true;
    ext::Entry item; item.key = "{3d1975af-48c6-4f8e-a182-be0e08fa86a9}";
    item.provider = "verb:" + item.key; item.label = L"Installed Control Panel"; item.token = 27;
    reply.entries = {item}; ext::Associate(catalogue, desktop, reply);
    Expect(reply.entries.front().registration == ext::NvidiaControlPanelRegistration,
        "compatibility uses the original handler identity in settings");
    ext::Preferences prefs;
    ext::SetCommon(prefs, ext::NvidiaControlPanelRegistration, ext::Category::Background, true);
    Expect(ext::VisibleSnapshot(prefs, reply, ext::ContextBit(ext::Context::Desktop)).size() == 1,
        "the existing registration switch shows the compatibility item");
    ext::SetOverride(prefs, ext::NvidiaControlPanelRegistration, ext::Context::Desktop, ext::Visibility::Hide);
    Expect(ext::VisibleSnapshot(prefs, reply, ext::ContextBit(ext::Context::Desktop)).empty(),
        "desktop visibility overrides still hide the compatibility item");

    class ApplicationMenu final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IContextMenu>
    {
    public:
        std::wstring verb = L"open";
        IFACEMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT) override { return E_NOTIMPL; }
        IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO) override { return E_NOTIMPL; }
        IFACEMETHODIMP GetCommandString(UINT_PTR offset, UINT flags, UINT *, LPSTR output, UINT size) override
        {
            if (offset != 27 || flags != GCS_VERBW || !output || size <= verb.size()) return E_INVALIDARG;
            wcscpy_s(reinterpret_cast<wchar_t *>(output), size, verb.c_str()); return S_OK;
        }
    };
    auto application = Microsoft::WRL::Make<ApplicationMenu>();
    struct Menu { HMENU value = CreatePopupMenu(); ~Menu() { DestroyMenu(value); } } menu;
    AppendMenuW(menu.value, MF_STRING, 1, L"Uninstall");
    AppendMenuW(menu.value, MF_STRING, 28, L"Open");
    SetMenuDefaultItem(menu.value, 28, FALSE);
    Expect(ext::DefaultApplicationOpen(application.Get(), menu.value) == 27u,
        "the safe application open command keeps its nonzero Shell offset, not the first command");
    SetMenuDefaultItem(menu.value, 1, FALSE);
    Expect(!ext::DefaultApplicationOpen(application.Get(), menu.value), "uninstall is never an application launcher");
    SetMenuDefaultItem(menu.value, 28, FALSE);
    application->verb = L"runas";
    Expect(!ext::DefaultApplicationOpen(application.Get(), menu.value), "elevation is never substituted for ordinary launch");
    application->verb = L"open";
    EnableMenuItem(menu.value, 28, MF_BYCOMMAND | MF_GRAYED);
    Expect(!ext::DefaultApplicationOpen(application.Get(), menu.value), "a default action disabled after query cannot run");
    EnableMenuItem(menu.value, 28, MF_BYCOMMAND | MF_ENABLED);
    SetMenuDefaultItem(menu.value, UINT(-1), FALSE);
    Expect(!ext::DefaultApplicationOpen(application.Get(), menu.value), "missing default action is not guessed");
}

void TestSourceAttribution()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::Entry entry; entry.provider = "menu:legacy"; entry.label = L"Legacy action";
    ext::Reply actual{true, {entry}, {}}, probe = actual;
    Expect(ext::MatchSourceCommands(actual, probe).empty(), "matching captions alone cannot attribute an application");
    entry.width = entry.height = 1; entry.pixels = {10, 20, 30, 255};
    actual.entries = probe.entries = {entry};
    Expect(ext::MatchSourceCommands(actual, probe) == std::vector<std::string>{entry.provider}, "exact legacy bitmap and menu structure provide source evidence");
    probe.entries.front().pixels[0] = 40;
    Expect(ext::MatchSourceCommands(actual, probe).empty(), "same-caption different-icon applications are not confused");
    probe = actual; actual.entries.push_back(entry);
    Expect(ext::MatchSourceCommands(actual, probe).empty(), "indistinguishable actual commands remain unattributed");
    actual.entries = {entry}; actual.entries.front().key = "CanonicalVerb";
    probe.entries.front().key = "canonicalverb"; probe.entries.front().label = L"Provider title";
    Expect(ext::MatchSourceCommands(actual, probe).size() == 1, "canonical provider command matches without relying on captions or casing");
    probe.ok = false;
    Expect(ext::MatchSourceCommands(actual, probe).empty(), "failed source query never contributes attribution");
    ext::Registration source; source.id = "clsid:first"; source.application = {"product:first", L"First app"};
    auto other = source; other.id = "clsid:other"; other.application = {"product:other", L"Other app"};
    ext::Catalogue catalogue; catalogue.rows = {source, other}; ext::Registration observed; observed.id = entry.provider;
    ext::RecordSourceApplication(observed, source, catalogue);
    Expect(observed.application == source.application && observed.id == entry.provider && observed.commandIdentity.empty(), "source metadata preserves the individual action switch identity");
    ext::RecordSourceApplication(observed, other, catalogue);
    Expect(observed.application.id.empty(), "conflicting runtime source claims are not attributed to either application");
    source.verbs = other.verbs = {"same"}; source.contexts = other.contexts = 1;
    source.types = other.types = {L"*"}; other.application = source.application;
    catalogue.rows = {source, other}; ext::Request request; request.context = ext::Context::File; entry.key = "SAME";
    Expect(ext::RegisteredApplication(catalogue, request, entry) == source.application, "duplicate registrations may agree on application without merging their command identities");
    catalogue.rows.back().application = {"different", L"Different"};
    Expect(ext::RegisteredApplication(catalogue, request, entry).id.empty(), "duplicate verb registrations from different applications remain ambiguous");
}
void TestManagementFilters()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::Catalogue catalogue;
    auto add = [&](const char *id, unsigned contexts, std::vector<std::wstring> types, ext::Application app, const char *command = "") {
        ext::Registration row; row.id = id; row.display.label = L"Action"; row.contexts = contexts;
        row.types = std::move(types); row.application = std::move(app); row.commandIdentity = command;
        row.linked = true; catalogue.rows.push_back(std::move(row));
    };
    const ext::Application editor{"app:editor", L"Editor"}, other{"app:other", L"Editor"};
    add("png", 1, {L".png"}, editor, "image-command");
    add("jpg", 1, {L".jpg"}, editor, "image-command");
    add("similar-suffix", 1, {L".pngx"}, editor);
    add("common", 3, {L"*"}, editor);
    add("other", 1, {L".png"}, other);
    add("unknown", 1, {L".png"}, {});
    add("folders", 2, {L"*"}, editor);
    add("background", 12, {L"*"}, editor);
    auto disabled = catalogue.rows.front(); disabled.id = "disabled"; disabled.systemEnabled = false; catalogue.rows.push_back(disabled);
    const auto filtered = ext::ManagementRows(catalogue, ext::Category::Objects, L"", editor.id, L"PNG");
    Expect(filtered.size() == 2, "application and exact case-insensitive suffix filters intersect, include generic file actions, and exclude folder-only actions");
    Expect(std::any_of(filtered.begin(), filtered.end(), [](const auto &r) { return r.members.size() == 2 && r.types == std::vector<std::wstring>{L".jpg", L".png"}; }),
        "suffix filtering keeps an existing merged action's complete switch scope visible");
    const auto unidentified = ext::ManagementRows(catalogue, ext::Category::Objects, L"", "@unknown", L".png");
    Expect(unidentified.size() == 1 && unidentified.front().id == "unknown", "unidentified application is an explicit filter, not a guessed caption match");
    Expect(ext::ManagementRows(catalogue, ext::Category::Objects, L"edITOR", other.id, L".png").size() == 1,
        "application names are searchable but equal names do not merge different provider identities");
    Expect(ext::ManagementRows(catalogue, ext::Category::Objects, L"absent", editor.id, L".png").empty(), "name search intersects the selected application and extension");
    Expect(ext::ManagementRows(catalogue, ext::Category::Objects, L"", editor.id, L"*").size() == 1, "generic-type filter selects only common file actions");
    Expect(ext::ManagementRows(catalogue, ext::Category::Background, L"", editor.id).size() == 1, "background filtering stays within its category");
    ext::Preferences prefs;
    ext::SetCommon(prefs, "common", ext::Category::Background, true);
    ext::SetOverride(prefs, "common", ext::Context::Folder, ext::Visibility::Hide);
    add("arrived-later", 1, {L".png"}, editor);
    ext::SetManagementResults(prefs, filtered, ext::Category::Objects, true);
    for (const auto *id : {"png", "jpg", "common"}) Expect(ext::CommonShown(prefs, id, ext::Category::Objects), "batch show updates each member of the captured filtered actions");
    for (const auto *id : {"other", "unknown", "similar-suffix", "folders", "disabled", "arrived-later"})
        Expect(ext::IsHidden(prefs, id, ext::Context::File), "batch show never expands to unmatched, disabled or newly discovered registrations");
    Expect(ext::OverrideOf(prefs, "common", ext::Context::Folder) == ext::Visibility::Inherit && ext::CommonShown(prefs, "common", ext::Category::Background),
        "explicit batch show restores location inheritance and preserves the other category");
    ext::Reply menu; menu.ok = true;
    for (const auto *id : {"png", "other", "arrived-later"}) { ext::Entry entry; entry.provider = id; entry.registration = id; menu.entries.push_back(entry); }
    Expect(ext::VisibleSnapshot(prefs, menu, 1).size() == 1, "batch-enabled preferences control the real popup visibility path");
    ext::SetManagementResults(prefs, filtered, ext::Category::Objects, false);
    Expect(ext::VisibleSnapshot(prefs, menu, 1).empty() && ext::CommonShown(prefs, "common", ext::Category::Background), "batch hide removes the filtered actions without changing background rules");
    const auto before = prefs; ext::SetManagementResults(prefs, {}, ext::Category::Objects, true);
    Expect(prefs == before, "empty filtered results never modify preferences");

    const auto base = ext::ManagementRows(catalogue, ext::Category::Objects);
    const auto applications = ext::ManagementCards(base, ext::Category::Objects, ext::ManagementView::Applications);
    Expect(applications.size() == 3 && applications.back().id == "app:@unknown", "application cards keep same-named providers separate and unidentified items in their own card");
    const auto types = ext::ManagementCards(base, ext::Category::Objects, ext::ManagementView::Extensions);
    const auto png = std::find_if(types.begin(), types.end(), [](const auto &card) { return card.title == L".png"; });
    const auto jpg = std::find_if(types.begin(), types.end(), [](const auto &card) { return card.title == L".jpg"; });
    Expect(png != types.end() && jpg != types.end() && jpg->rows.size() == 1, "type views include merged actions in each supported suffix card");
    Expect(std::none_of(png->rows.begin(), png->rows.end(), [](const auto &row) {
        return std::any_of(row.members.begin(), row.members.end(), [](const auto &member) { return member.id == "common" || member.id == "folders"; });
    }), "common and folder actions have dedicated cards instead of repeating under every suffix");
    ext::Preferences cardPreferences;
    ext::SetOverride(cardPreferences, "png", ext::Context::File, ext::Visibility::Hide);
    ext::SetManagementResults(cardPreferences, png->rows, ext::Category::Objects, true);
    Expect(ext::ManagementCardState(cardPreferences, *jpg, ext::Category::Objects) == true &&
        ext::OverrideOf(cardPreferences, "png", ext::Context::File) == ext::Visibility::Inherit, "group toggles share original state across type cards and restore location inheritance");
    const auto application = std::find_if(applications.begin(), applications.end(), [&](const auto &card) { return card.id == "app:" + editor.id; });
    Expect(application != applications.end() && !ext::ManagementCardState(cardPreferences, *application, ext::Category::Objects), "partly shown application cards report mixed state");
    ext::SetManagementResults(cardPreferences, application->rows, ext::Category::Objects, true);
    Expect(ext::ManagementCardState(cardPreferences, *application, ext::Category::Objects) == true &&
        std::none_of(cardPreferences.rules.begin(), cardPreferences.rules.end(), [](const auto &rule) { return rule.id.find('\n') != std::string::npos; }), "card identities never replace persisted action IDs");
}

// The real presenter reconciliation planner drives a minimal control adapter;
// only WinUI widgets are replaced. A clear/recreate regression loses identity.
void TestManagementUpdates()
{
    namespace ext = snowdesktop::shell_extensions;
    struct Control { ext::ManagementRow model; int identity = 0; bool expanded = false; };
    std::vector<Control> controls;
    int created = 0, removed = 0;
    auto apply = [&](const std::vector<ext::ManagementRow> &desired) {
        const auto updates = ext::PlanManagementUpdates(controls, desired, [&](auto &) { ++removed; });
        for (const auto &model : updates)
        {
            const auto old = std::find_if(controls.begin(), controls.end(), [&](const auto &r) { return r.model.id == model.id; });
            if (old == controls.end()) controls.push_back({model, ++created, false});
            else old->model = model;
        }
        return updates.size();
    };
    ext::ManagementRow first; first.id = "first"; first.display.label = L"First"; first.contexts = 1;
    first.members = {{"reg:png", 1}}; first.types = {L".png"};
    Expect(apply({first}) == 1 && created == 1, "initial discovery creates one row");
    controls.front().expanded = true;
    auto irrelevant = first; irrelevant.display.token = 37; irrelevant.display.key = "later-session";
    Expect(apply({irrelevant}) == 0, "query tokens and ownership metadata never rebuild an unchanged settings control");
    auto second = first; second.id = "second"; second.display.label = L"Second";
    Expect(apply({first, second}) == 1 && created == 2 && removed == 0 && controls.front().identity == 1 && controls.front().expanded,
        "incremental discovery preserves existing control identity and expansion");
    first.types.push_back(L".jpg"); first.members.push_back({"reg:jpg", 1});
    Expect(apply({first, second}) == 1 && created == 2 && removed == 0 && controls.front().expanded,
        "new file-type membership updates its row in place");
    first.display.width = first.display.height = 1; first.display.pixels = {0, 0, 0, 255};
    Expect(apply({first, second}) == 1 && created == 2, "a newly available icon changes only its surviving row");
    first.applications = {{"app:provided", L"Providing app"}};
    Expect(apply({first, second}) == 1 && created == 2 && controls.front().expanded, "late application metadata updates the row without resetting its expansion");
    Expect(apply({first}) == 0 && removed == 1 && controls.front().expanded,
        "removal retires only the missing identity, keeping expansion and focus targets of survivors");
}

void TestExtensionSessions()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::InvalidateMenuCache();
    ext::Entry archive;
    archive.provider = "verb:sevenzip";
    archive.key = "SevenZip";
    archive.label = L"7-Zip";
    ext::Entry command; command.key="compress"; command.label=L"压缩"; command.token=31;
    command.checked=true; command.enabled=false;
    archive.children={command};
    ext::Entry other; other.provider="verb:editor"; other.label=L"Editor"; other.token=32;
    ext::Preferences prefs;
    ext::Request target; target.context=ext::Context::File;
    Expect(ext::VisibleEntries(prefs, {archive, other}, target).empty(),
           "fresh settings hide all extension items");
    for (auto context : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
    {
        prefs.shown.clear();
        ext::SetHidden(prefs, archive.provider, context, false);
        for (auto current : {ext::Context::File, ext::Context::Folder, ext::Context::FolderBackground, ext::Context::Desktop})
        {
            target.context=current;
            const auto shown=ext::VisibleEntries(prefs,{archive,other},target);
            Expect(shown.size() == (current == context ? 1u : 0u),
                   "explicit visibility is independent across all four contexts");
        }
        target.context = context;
        const auto direct = ext::VisibleEntries(prefs, {archive, other}, target);
        Expect(direct.size() == 1 && direct[0].label == L"7-Zip" && direct[0].children.size() == 1 &&
                   direct[0].children[0].token == 31 && direct[0].children[0].checked &&
                   !direct[0].children[0].enabled,
               "enabled system items retain roots, submenus and disabled states without wrappers");
        archive.label = L"Localized title";
        Expect(ext::VisibleEntries(prefs, {archive}, target).size() == 1,
               "canonical visibility survives a display-name change");
        archive.label = L"7-Zip";
        ext::SetHidden(prefs, archive.provider, context, true);
        Expect(ext::VisibleEntries(prefs, {archive}, target).empty(), "turning an item off hides it again");
    }
    ext::SetHidden(prefs, archive.provider, target.context, false);
    Expect(ext::VisibleEntries(prefs, {}, target).empty(),
           "an opt-in never resurrects an item absent from the Shell");
    auto wait=[](ext::Session& session) {
        const auto end=GetTickCount64()+10000;std::optional<ext::Reply> reply;
        while(GetTickCount64()<end&&!reply)
        {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            reply=session.Poll();if(!reply)MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(reply.has_value(),"isolated query has a bounded completion");return *reply;
    };
    {
        // Exercise the real process launcher while the caller owns a redirected
        // stdout pipe. Third-party query code must be unable to retain/write it.
        struct OutputPipe
        {
            HANDLE original = GetStdHandle(STD_OUTPUT_HANDLE), read = nullptr, write = nullptr;
            ~OutputPipe()
            {
                SetStdHandle(STD_OUTPUT_HANDLE, original);
                if (read)
                    CloseHandle(read);
                if (write)
                    CloseHandle(write);
            }
        } output;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Expect(CreatePipe(&output.read, &output.write, &security, 0) != FALSE, "private stdout probe pipe");
        Expect(SetStdHandle(STD_OUTPUT_HANDLE, output.write) != FALSE,
               "redirect only the probe caller's stdout");
        ext::Request probe;
        probe.paths = {L"synthetic-stdio"};
        ext::Session session(probe, 2500);
        SetStdHandle(STD_OUTPUT_HANDLE, output.original);
        const auto reply = wait(session);
        DWORD available = 0;
        Expect(PeekNamedPipe(output.read, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
                   available == 0 && reply.ok && reply.entries.front().key == "isolated-stdio",
               "Shell helpers cannot inherit or write the caller's redirected output stream");
    }
    ext::Request request;request.paths={L"synthetic-success"};
    DWORD firstProcess = 0;
    {ext::Session session(request,2500);auto reply=wait(session); firstProcess = session.ProcessId();
        Expect(reply.ok&&reply.entries.size()==1&&reply.entries[0].label==L"压缩"&&reply.entries[0].checked&&!reply.entries[0].enabled,
            "isolated query transports Unicode labels and actual menu states");}
    request.paths={L"synthetic-second"};
    {ext::Session session(request,2500);auto reply=wait(session);
        Expect(session.ProcessId()==firstProcess && reply.ok && reply.entries[0].key=="second",
            "a warm worker is reused but queries the new selection instead of reusing old commands");}
    {
        ext::Session first(request,2500); Expect(wait(first).ok,"first concurrent query");
        ext::Session second(request,2500); Expect(wait(second).ok,"second concurrent query");
        Expect(first.ProcessId()!=second.ProcessId(),"active menu sessions never share a worker or command map");
        bool refused = false;
        try { ext::Session third(request, 2500); } catch (const snowdesktop::settings_ipc::ProtocolError &) { refused = true; }
        Expect(refused, "arbitrary extensions retain their two-session limit");
        auto start = request; start.startPinOnly = true;
        ext::Session reserved(start, 2500); const auto direct = wait(reserved);
        Expect(direct.ok && reserved.ProcessId() != first.ProcessId() && reserved.ProcessId() != second.ProcessId(),
            "Start queries have an isolated reserved helper even while both ordinary slots are occupied");
    }
    ext::InvalidateMenuCache();
    {ext::Session session(request,2500);auto reply=wait(session);
        Expect(session.ProcessId()!=firstProcess && reply.ok,"explicit refresh discards the previous cached worker");}
    {
        ext::Request sample; sample.catalogueOnly=true; sample.context=ext::Context::File;
        std::wstring samplePath;
        DWORD sampleWorker=0;
        {
            ext::Session session(sample,2500); auto reply=wait(session);
            Expect(reply.ok,"owned catalogue sample query completes");
            samplePath=reply.entries[0].label; sampleWorker=session.ProcessId();
            Expect(GetFileAttributesW(samplePath.c_str())!=INVALID_FILE_ATTRIBUTES,"sample lives through its menu session");
        }
        // Reacquire immediately to exercise a release acknowledgement arriving
        // after the next request; it must not remove the current sample.
        ext::Session next(sample,2500); auto reply=wait(next);
        Expect(next.ProcessId()==sampleWorker && reply.ok &&
            GetFileAttributesW(samplePath.c_str())==INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(reply.entries[0].label.c_str())!=INVALID_FILE_ATTRIBUTES,
            "release acknowledgements remove retired samples while preserving the next session's object");
    }
    request.paths={L"synthetic-hang"};const auto start=GetTickCount64();
    {ext::Session session(request,250);auto reply=wait(session);Expect(!reply.ok&&GetTickCount64()-start<3000,"hung query is terminated without blocking the parent");}
    request.paths={L"synthetic-success"};
    {ext::Session session(request,2500);Expect(wait(session).ok,"a timed-out extension does not poison the next menu session");}
    for(int i=0;i<3;++i){ext::Session cancelled(request);}
    // Exercise the production aggregate against the installed system menus,
    // on an isolated directory and without invoking a file operation.
    TemporaryDirectory directory;
    request.paths={directory.path.wstring()};request.background=false;
    struct RealQueryMode
    {
        RealQueryMode(){ext::InvalidateMenuCache(); SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",L"1");}
        ~RealQueryMode(){ext::InvalidateMenuCache(); SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",nullptr);}
    } realMode;
    TestSystemPolicy(wait, directory.path);
    // Exercise the same four default queries as the settings tabs, including
    // real installed handler images. Report every scope before failing so one
    // incompatible DLL cannot hide the remaining scope results.
    bool scopesPassed = true;
    for (const auto context : {ext::Context::File, ext::Context::Folder,
                              ext::Context::FolderBackground, ext::Context::Desktop})
    {
        ext::Request sample; sample.catalogueOnly = true; sample.context = context;
        ext::Session actual(sample);
        const auto reply = wait(actual);
        std::cout << "System scope " << static_cast<int>(context) << ": "
                  << (reply.ok ? "queried" : "FAILED") << ", entries=" << reply.entries.size()
                  << ", " << reply.error << std::endl;
        scopesPassed &= reply.ok;
        for (const auto &entry : reply.entries)
            if (entry.label.find(L"PowerShell") != std::wstring::npos)
            {
                std::cout << "PowerShell cascade: native=" << entry.native
                          << ", children=" << entry.children.size() << std::endl;
                scopesPassed &=
                    std::none_of(entry.children.begin(), entry.children.end(), [](const auto &child) {
                        return child.label.empty() || child.label == L"…" || child.label == L"...";
                    });
            }
        if (!reply.ok || (context != ext::Context::File && context != ext::Context::Folder))
            continue;
        const auto archiveEntry = std::find_if(reply.entries.begin(), reply.entries.end(), [](const auto &entry) {
            return entry.label == L"7-Zip";
        });
        // Registration alone does not imply system visibility: 7-Zip may be
        // disabled. The isolated verb above independently checks visibility.
        if (archiveEntry != reply.entries.end())
        {
            std::cout << "7-Zip menu image: " << archiveEntry->width << "x" << archiveEntry->height
                      << ", " << archiveEntry->pixels.size() << " bytes" << std::endl;
            scopesPassed &= !archiveEntry->children.empty() && archiveEntry->width > 0 && archiveEntry->height > 0 &&
                            !archiveEntry->pixels.empty();
        }
        else std::cout << "7-Zip is not present in this system menu; icon check not applicable" << std::endl;
    }
    Expect(scopesPassed, "all four real settings scopes query successfully and preserve installed 7-Zip icons");

    // Windows' packaged Terminal registration was missing only from the first
    // aggregate in each helper. Compare the same unchanged desktop across a
    // cold and warm request; never invoke Terminal or depend on its local title.
    for (int coldStart = 0; coldStart < 2; ++coldStart)
    {
        ext::InvalidateMenuCache();
        ext::Request desktop;
        desktop.catalogueOnly = true;
        desktop.context = ext::Context::Desktop;
        ext::Reply first, second;
        {
            ext::Session cold(desktop);
            first = wait(cold);
        }
        {
            ext::Session warm(desktop);
            second = wait(warm);
        }
        Expect(first.ok && second.ok, "cold and warm desktop queries complete");
        const auto terminal = [](const auto &entries) {
            return std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
                return entry.provider == "verb:{9f156763-7844-4dc4-b2b1-901f640f5155}";
            });
        };
        std::cout << "Cold desktop Terminal: first=" << terminal(first.entries)
                  << ", warm=" << terminal(second.entries) << std::endl;
        Expect(!terminal(second.entries) || terminal(first.entries),
               "Terminal available in the system must already appear in the first cold desktop query");
    }
}

void TestCatalogueCache()
{
    namespace ext = snowdesktop::shell_extensions;
    const auto escaped=snowdesktop::DecodeMenuLabel(L"研发 && 工具(&T)\tCtrl+T");
    Expect(escaped.text==L"研发 & 工具(T)\tCtrl+T" && escaped.accessKey==L't',
        "native text preserves escaped ampersands and records the marked access key");
    Expect(snowdesktop::DecodeMenuLabel(L"file(A) && B\tCtrl+&C").accessKey==0,
        "literal names and shortcut-column text never invent access keys");
    ext::Entry transport; transport.accessKey=L'a';
    const auto packed=snowdesktop::settings_ipc::Pack(transport);
    Expect(snowdesktop::settings_ipc::Unpack<ext::Entry>(packed).accessKey==L'a',
        "native access keys survive the real Shell IPC codec");
    TemporaryDirectory directory;
    ext::MenuSnapshotCache writer(directory.path / L"cache");
    ext::Request file; file.catalogueOnly=true; file.context=ext::Context::File;
    const auto path = directory.path / L"sample.txt";
    { std::ofstream output(path); output << "sample"; }
    file.paths = {path.wstring()};
    ext::Reply reply; reply.ok=true;
    ext::Entry entry; entry.label=L"7-Zip"; entry.provider="archive"; entry.token=12;
    entry.width=1;entry.height=1;entry.pixels={12,24,36,255};
    ext::Entry child; child.token=13;child.key="compress";child.label=L"压缩";
    entry.children={child}; reply.entries={entry};
    const auto ticket=writer.Capture(file);
    Expect(writer.Store(ticket,reply,100),"write display snapshot to private disk cache");
    ext::MenuSnapshotCache reader(directory.path / L"cache");
    file.catalogueOnly=false; // The host reads a settings-written exact-object snapshot.
    const auto hostTicket=reader.Capture(file);
    const auto hit=reader.Find(hostTicket,101);
    // A separate helper process reads the settings-written file directly. Only
    // its query boundary is replaced, not serialization or the disk reader.
    ext::Request crossProcess;crossProcess.paths={L"read-disk-cache",writer.Directory().wstring(),path.wstring()};
    {
        ext::Session helper(crossProcess);
        std::optional<ext::Reply> remote;
        const auto deadline=GetTickCount64()+10000;
        while(!remote&&GetTickCount64()<deadline)
        {
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
            remote=helper.Poll();
            if(!remote) MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        Expect(remote&&remote->ok&&remote->entries.size()==1&&remote->entries[0].pixels==entry.pixels&&
            !remote->entries[0].token,"another process reads the same disk snapshot and icons without native tokens");
    }
    Expect(hit && hit->entries[0].label==L"7-Zip" && !hit->entries[0].token && !hit->entries[0].children[0].token &&
        hit->entries[0].pixels==entry.pixels,"disk reload shares text, hierarchy and icons but never Shell tokens");
    const auto reference=ext::AppendReference(ext::AppendReference({},entry),child);
    reply.entries[0].children[0].token=72;
    Expect(ext::ResolveCommand(reply,reference)==72,"cached selection resolves the fresh command token, not the saved token");
    reply.entries[0].children[0].enabled=false;
    Expect(!ext::ResolveCommand(reply,reference),"disabled fresh commands cannot execute through a cache hit");
    reply.entries[0].children[0].enabled=true;reply.entries[0].enabled=false;
    Expect(!ext::ResolveCommand(reply,reference),"disabled ancestor blocks cached child invocation");
    reply.entries[0].enabled=true;reply.entries.push_back(reply.entries[0]);
    Expect(!ext::ResolveCommand(reply,reference),"ambiguous menu identities cannot select a command by position");
    reply.entries.resize(1);reply.entries[0].children[0].key="different";
    Expect(!ext::ResolveCommand(reply,reference),"a reused position with another verb cannot execute the old selection");
    const auto other = directory.path / L"other.txt";
    { std::ofstream output(other); output << "sample"; }
    auto different=file;different.paths={other.wstring()};
    Expect(!reader.Find(reader.Capture(different),101),"same-extension files never share contextual command trees");
    different=file;different.extended=true;
    Expect(!reader.Find(reader.Capture(different),101),"Shift extended menus have their own snapshots");
    auto sample=file;sample.paths.clear();sample.catalogueOnly=true;
    Expect(!reader.Find(reader.Capture(sample),101),"settings samples do not replace exact-object menus");
    Expect(writer.Store(writer.Capture(sample),reply,100),"sample catalogue uses the same bounded disk store");
    sample.context=ext::Context::Folder;
    Expect(!reader.Find(reader.Capture(sample),101),"settings scopes have independent sample catalogues");
    Expect(!reader.Find(hostTicket,100+ext::MenuSnapshotCache::LifetimeMs),"expired disk snapshots are not presented");
    Expect(!reader.Find(hostTicket,99),"clock rollback rejects snapshots from the future");
    writer.Invalidate();
    Expect(!reader.Find(hostTicket,101)&&!reader.Find(reader.Capture(file),101),"another process invalidates disk and memory views");
    Expect(!writer.Store(ticket,reply,102),"an old in-flight query cannot restore invalidated cache data");
    const auto changedTicket=writer.Capture(file);
    { std::ofstream output(path,std::ios::app); output << "changed"; }
    Expect(!writer.Store(changedTicket,reply,102),"target changes during a query invalidate its pending snapshot");
    const auto current=writer.Capture(file);
    Expect(writer.Store(current,reply,103),"write current target snapshot");
    auto older = writer.Begin(current), newer = writer.Begin(current);
    newer.dependency = 37;
    Expect(writer.Store(newer, reply, 103) && !writer.Store(older, reply, 103), "older completion cannot overwrite a newer request sequence");
    auto differentDependency = current; differentDependency.dependency = 38;
    Expect(!reader.Find(differentDependency, 104), "disk snapshots from another registration dependency are misses");
    { std::ofstream output(directory.path / L"cache" / current.file,std::ios::binary);output<<"truncated"; }
    ext::MenuSnapshotCache afterRestart(directory.path / L"cache");
    Expect(!afterRestart.Find(current,104),"corrupt disk cache falls back to a live query");

    ext::Request desktop;
    desktop.paths = {directory.path.wstring()};
    desktop.background = true;
    desktop.context = ext::Context::Desktop;
    const auto desktopTicket = writer.Capture(desktop);
    Expect(writer.Store(desktopTicket, reply, 200), "seed a desktop display snapshot");
    {
        std::ofstream childFile(directory.path / L"new-child.txt");
        childFile << "desktop content changed";
    }
    // Force a different directory write time so the regression cannot depend
    // on filesystem timestamp granularity or a fixed sleep.
    const auto oldTime = std::filesystem::last_write_time(directory.path);
    std::filesystem::last_write_time(directory.path, oldTime + std::chrono::seconds(2));
    Expect(reader.Find(reader.Capture(desktop), 201).has_value(),
           "adding desktop files does not discard its display snapshot");
    for (int i = 0; i < 270; ++i)
    {
        const auto sibling = directory.path / (L"cache-collision-" + std::to_wstring(i) + L".txt");
        {
            std::ofstream output(sibling);
            output << i;
        }
        ext::Request object;
        object.paths = {sibling.wstring()};
        Expect(writer.Store(writer.Capture(object), reply, 200), "fill unrelated selection snapshots");
    }
    auto extendedDesktop = desktop;
    extendedDesktop.extended = true;
    auto extendedReply = reply;
    extendedReply.entries[0].label = L"Shift desktop";
    Expect(writer.Store(writer.Capture(extendedDesktop), extendedReply, 200),
           "seed extended desktop snapshot");
    const auto retained = reader.Find(reader.Capture(desktop), 201);
    Expect(retained && retained->entries[0].label == reply.entries[0].label,
           "unrelated selections and Shift menus cannot evict the ordinary desktop snapshot");
    size_t snapshots = 0;
    for (const auto &cacheFile : std::filesystem::directory_iterator(writer.Directory()))
        if (cacheFile.path().extension() == L".bin" && cacheFile.path().filename() != L"epoch.bin") ++snapshots;
    Expect(snapshots <= ext::MenuSnapshotCache::DiskEntries, "full-key disk cache evicts past its 256-entry budget");
}

template<class Condition>
void PumpUntil(Condition condition, const char *message)
{
    const auto deadline=GetTickCount64()+10000;
    while (!condition() && GetTickCount64()<deadline)
    {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg);DispatchMessageW(&msg); }
        if (!condition()) MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    Expect(condition(),message);
}

void TestExposedStartPinHelper()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace pin = snowdesktop::shell_start_pin;
    TemporaryDirectory directory;
    const auto output = directory.path / L"start-pin-result.bin";
    // Exercise the real Session IPC, dedicated menu, token lookup and Host
    // invocation. Substitute only the final Explorer call so no user pin changes.
    struct Mode
    {
        explicit Mode(const std::filesystem::path &path)
        {
            ext::InvalidateMenuCache();
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_START_PIN", path.c_str());
        }
        ~Mode()
        {
            ext::InvalidateMenuCache();
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr);
            SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_START_PIN", nullptr);
        }
    } mode(output);
    wchar_t target[32768]{};
    Expect(GetModuleFileNameW(nullptr, target, static_cast<DWORD>(std::size(target))) != 0,
        "isolated shortcut target is the test executable");
    const auto shortcut = directory.path / L"微信 菜单验证.lnk";
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    Expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(target)) && SUCCEEDED(link.As(&persist)) &&
        SUCCEEDED(persist->Save(shortcut.c_str(), TRUE)), "create a private Unicode shortcut");
    ext::Request request; request.paths = {shortcut.wstring()};
    request.startPinOnly = true;
    ext::Session session(request);
    std::optional<ext::Reply> reply;
    PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); },
        "the production helper queries the shortcut menu");
    Expect(reply->ok && reply->entries.size() == 1, "the real shortcut query exposes only its current Start command");
    const auto selected = std::find_if(reply->entries.begin(), reply->entries.end(), [](const auto &entry) {
        return entry.enabled && entry.token && !entry.native &&
            (_stricmp(entry.key.c_str(), "PinToStartScreen") == 0 ||
             _stricmp(entry.key.c_str(), "UnpinFromStartScreen") == 0);
    });
    Expect(selected != reply->entries.end(), "Windows exposes a canonical Start command for the shortcut");
    const auto expected = _stricmp(selected->key.c_str(), "PinToStartScreen") == 0 ? pin::Action::Pin : pin::Action::Unpin;
    bool rejected = false;
    try { session.Invoke(selected->token, {123, 456}); }
    catch (const snowdesktop::settings_ipc::ProtocolError &) { rejected = true; }
    Expect(rejected, "the final executor's failure reaches the stateful invocation caller");
    Expect(std::filesystem::exists(output),
        "an exposed Start command must reach the Explorer executor rather than local InvokeCommand");
    std::ifstream file(output, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto [path, action, ownerValid, x, y] = snowdesktop::settings_ipc::Unpack<
        std::tuple<std::wstring, pin::Action, bool, LONG, LONG>>(
            {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
    Expect(path == shortcut.wstring() && action == expected && ownerValid && x == 123 && y == 456,
        "the exposed menu preserves the original Unicode .lnk, action, helper owner and click position");
}

void TestPairedCommandVisibility()
{
    namespace ext = snowdesktop::shell_extensions;
    ext::Catalogue catalogue;
    ext::Preferences legacy;
    for (const auto &pair : ext::StateCommandPairs)
    {
        const auto forward = "verb:" + std::string(pair.forward), reverse = "verb:" + std::string(pair.reverse);
        legacy.rules.push_back({forward, ext::Category::Objects, false});
        legacy.rules.push_back({reverse, ext::Category::Objects, true});
        legacy.overrides.push_back({forward, ext::Context::File, ext::Visibility::Hide});
        legacy.overrides.push_back({reverse, ext::Context::File, ext::Visibility::Show});
        for (const auto &verb : {pair.forward, pair.reverse})
        {
            ext::Registration row; row.id = "verb:" + std::string(verb); row.linked = true; row.contexts = 3;
            row.display.key = verb; row.display.label = std::wstring(verb.begin(), verb.end());
            catalogue.rows.push_back(row);
        }
        Expect(!ext::IsHidden(legacy, forward, ext::Context::File) && !ext::IsHidden(legacy, reverse, ext::Context::File),
            "an opt-in from either legacy state variant applies to both, independently of record order");
    }
    const auto rows = ext::ManagementRows(catalogue, ext::Category::Objects);
    Expect(rows.size() == 3, "Start, taskbar and Quick Access each have one paired management switch");
    ext::Normalize(legacy);
    Expect(legacy.rules.size() == 3 && legacy.overrides.size() == 3,
        "legacy state aliases collapse to one authority per category and location");
    for (const auto &row : rows)
    {
        Expect(row.members.size() == 1 && ext::ManagementCommon(legacy, row, ext::Category::Objects) == true,
            "both discovered variants share the same management state");
        ext::SetManagementCommon(legacy, row, ext::Category::Objects, false);
        const auto *pair = ext::StatePairForId(row.id);
        Expect(pair && ext::IsHidden(legacy, "verb:" + std::string(pair->forward), ext::Context::File) &&
            ext::IsHidden(legacy, "verb:" + std::string(pair->reverse), ext::Context::Folder),
            "the primary hide switch disables both variants and clears old object exceptions");
        ext::SetManagementCommon(legacy, row, ext::Category::Objects, true);
        ext::SetOverride(legacy, "verb:" + std::string(pair->reverse), ext::Context::File, ext::Visibility::Hide);
        Expect(ext::IsHidden(legacy, "verb:" + std::string(pair->forward), ext::Context::File) &&
            !ext::IsHidden(legacy, "verb:" + std::string(pair->reverse), ext::Context::Folder),
            "paired location overrides preserve the independent file/folder scopes");
    }
    JsonValue saved;
    Expect(ParseJson(ext::WritePreferences(legacy), saved) && ext::ReadPreferences(&saved) == legacy,
        "the unified pair settings survive the real preferences serializer");
    ext::Preferences isolated;
    ext::SetCommon(isolated, "reg:folder\\shell\\pintohome", ext::Category::Objects, true);
    Expect(!ext::IsHidden(isolated, "verb:unpinfromhome", ext::Context::Folder) &&
        ext::IsHidden(isolated, "verb:other", ext::Context::Folder),
        "proven static Windows aliases join their pair without enabling unrelated commands");
    const auto forward = catalogue.rows.front().display;
    auto changed = forward; changed.key = "UnpinFromStartScreen"; changed.provider = "verb:unpinfromstartscreen"; changed.token = 99;
    ext::Reply fresh{true, {changed}, {}};
    Expect(ext::ResolveCommand(fresh, ext::AppendReference({}, forward)) == 0,
        "shared visibility must never resolve an old Pin click to the opposite Unpin action");
}

void TestPairedCommandRefresh()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    const auto target = directory.path / L"state.lnk";
    std::ofstream(target) << "private scheduler target";
    ext::Request request; request.paths = {target.wstring()};
    std::atomic<bool> pinned = false, hold = false, completed = false, success = false;
    std::atomic<int> queries = 0, ordinaryQueries = 0, invokes = 0;
    auto startRequest = request; startRequest.startPinOnly = true;
    ext::MenuService service(directory.path / L"cache", [&](const auto &source) {
        ++queries;
        if (!source.startPinOnly) ++ordinaryQueries;
        const bool state = pinned.load();
        ext::Entry action; action.key = state ? "UnpinFromStartScreen" : "PinToStartScreen";
        action.provider = state ? "verb:unpinfromstartscreen" : "verb:pintostartscreen";
        action.label = state ? L"Unpin" : L"Pin"; action.token = state ? 42 : 41;
        ext::Entry ordinary; ordinary.key = "inspect"; ordinary.provider = "verb:inspect"; ordinary.label = L"Inspect"; ordinary.token = 43;
        return ext::QueryWork{[&, action, ordinary, direct = source.startPinOnly]() -> std::optional<ext::Reply> {
            if (hold) return {}; return ext::Reply{true, direct ? std::vector<ext::Entry>{action} : std::vector<ext::Entry>{ordinary, action}, {}};
        }, [&, state, token = action.token](UINT selected, POINT) {
            if (selected != token || pinned.load() != state) throw std::runtime_error("wrong state token");
            ++invokes; pinned = !state; hold = true; // Hold the post-action query, never the invocation.
        }};
    }, [] { return ext::Catalogue{}; });
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:pintostartscreen", ext::Category::Objects, true);
    ext::SetCommon(prefs, "verb:inspect", ext::Category::Objects, true);
    service.Configure(prefs);
    service.Query(request);
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "seed the pre-action menu snapshot");
    pinned = true; hold = true; // State changes outside SnowDesktop, without changing the .lnk.
    menu::Item more; more.command = 7; more.label = L"More";
    ext::Presentation presentation(request, prefs, L"", L"", service, [&](bool ok) { success = ok; completed = true; });
    std::vector<menu::Item> items{more}; menu::Options options;
    presentation.Attach(items, options, 7);
    Expect(items.size() == 2 && items.front().label == L"Inspect" && options.pollItems &&
        std::none_of(items.begin(), items.end(), [](const auto &item) { return item.label == L"Pin"; }),
        "warm ordinary commands appear immediately, but cached external pin state is never displayed");
    PumpUntil([&] { return queries >= 2; }, "opening a warm paired menu forces a native state query");
    Expect(!options.pollItems(items, true), "a held state query cannot reinsert the stale cached action");
    hold = false;
    PumpUntil([&] { return !service.View(startRequest).pending && service.View(startRequest).snapshot.has_value(); }, "fresh external state query completes");
    const auto additions = options.pollItems(items, true);
    Expect(additions && std::count_if(additions->begin(), additions->end(), [](const auto &i) { return i.label == L"Unpin"; }) == 1 &&
        std::none_of(additions->begin(), additions->end(), [](const auto &i) { return i.label == L"Pin"; }),
        "this opening materializes exactly the current Unpin state without duplicating ordinary rows");
    const auto selected = std::find_if(additions->begin(), additions->end(), [](const auto &i) { return i.label == L"Unpin"; });
    Expect(presentation.Invoke(selected->command, {0, 0}), "dispatch the freshly displayed state command");
    PumpUntil([&] { return completed.load(); }, "state invocation completion is observed by the real service");
    const auto ordinaryView = service.View(request);
    Expect(success && invokes == 1 && !pinned && ordinaryQueries == 1 && ordinaryView.snapshot &&
        ordinaryView.snapshot->entries.size() == 1 && ordinaryView.snapshot->entries.front().label == L"Inspect",
        "a Start state change retires the old pin while preserving ordinary rows without an aggregate query");
    ext::MenuSnapshotCache disk(directory.path / L"cache");
    const auto ordinaryDisk = disk.Find(disk.Capture(request));
    Expect(ordinaryDisk && ordinaryDisk->entries.size() == 1 && ordinaryDisk->entries.front().label == L"Inspect",
        "the disk snapshot preserves ordinary rows and cannot restore the stale pin state");
    hold = false;
    PumpUntil([&] { const auto view = service.View(startRequest); return !view.pending && view.snapshot.has_value(); },
        "the post-action native query publishes the new Pin state");
    const auto after = service.MenuDisplay(startRequest, prefs);
    Expect(after.snapshot && after.snapshot->entries.back().label == L"Pin" && invokes == 1 && ordinaryQueries == 1,
        "one Unpin action produces one fresh Pin state without repeating aggregate enumeration or invocation");
}

void TestStartQueryScheduling()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    ext::Request first, second, target;
    first.paths = {(directory.path / L"first.lnk").wstring()};
    second.paths = {(directory.path / L"second.lnk").wstring()};
    target.paths = {(directory.path / L"target.lnk").wstring()};
    for (const auto &request : {first, second, target}) std::ofstream(std::filesystem::path(request.paths.front())) << "isolated target";
    std::atomic<bool> hold = true;
    std::atomic<unsigned> ordinaryQueries = 0, startQueries = 0;
    // Only the native process boundary is substituted. Exercise the production
    // scheduler, popup bridge and cache with two deliberately stalled extensions.
    ext::MenuService service(directory.path / L"cache", [&](const auto &request) {
        const bool direct = request.startPinOnly;
        if (direct) ++startQueries; else ++ordinaryQueries;
        ext::Entry entry; entry.token = direct ? 1 : 2;
        entry.key = direct ? "PinToStartScreen" : "inspect";
        entry.provider = direct ? "verb:pintostartscreen" : "verb:inspect";
        entry.label = direct ? L"Pin" : L"Inspect";
        return ext::QueryWork{[&, direct, entry]() -> std::optional<ext::Reply> {
            if (!direct && hold) return {}; return ext::Reply{true, {entry}, {}};
        }, {}};
    }, [] { return ext::Catalogue{}; });
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:pintostartscreen", ext::Category::Objects, true);
    ext::SetCommon(prefs, "verb:inspect", ext::Category::Objects, true);
    service.Configure(prefs);
    service.Query(first); service.Query(second);
    PumpUntil([&] { return ordinaryQueries == 2; }, "occupy both ordinary extension slots");
    // Model a target disappearing after selection: no cache ticket can be
    // captured. The process-boundary substitute still completes, so lane
    // accounting must remain correct without a persistent cache identity.
    std::filesystem::remove(std::filesystem::path(target.paths.front()));
    ext::Presentation presentation(target, prefs, L"", L"", service);
    menu::Item builtin; builtin.command = 5; builtin.label = L"Open";
    menu::Item more; more.command = 7; more.label = L"More";
    std::vector<menu::Item> items{builtin, more}; menu::Options options;
    presentation.Attach(items, options, 7);
    auto start = target; start.startPinOnly = true;
    PumpUntil([&] { return service.View(start).snapshot.has_value(); }, "the reserved Start query bypasses both stalled extensions");
    const auto early = options.pollItems(items, true);
    Expect(ordinaryQueries == 2 && startQueries == 1 && early && early->size() == 4 &&
        std::count_if(early->begin(), early->end(), [](const auto &i) { return i.label == L"Pin"; }) == 1 &&
        !options.pollItemsFinished(), "Start appears before the cold aggregate without ending its independent publication");
    items = *early;
    hold = false;
    PumpUntil([&] { return service.View(target).snapshot.has_value(); }, "the cold aggregate can finish afterwards");
    const auto late = options.pollItems(items, true);
    Expect(late && options.pollItemsFinished() && late->back().command == 7 &&
        std::count_if(late->begin(), late->end(), [](const auto &i) { return i.label == L"Pin"; }) == 1 &&
        std::count_if(late->begin(), late->end(), [](const auto &i) { return i.label == L"Inspect"; }) == 1,
        "later ordinary publication preserves the Start action and the footer without duplicates");
    const auto previousOrdinary = ordinaryQueries.load();
    ext::SetCommon(prefs, "verb:inspect", ext::Category::Objects, false); service.Configure(prefs);
    ext::Presentation onlyStart(target, prefs, L"", L"", service);
    std::vector<menu::Item> fresh{more}; menu::Options freshOptions; onlyStart.Attach(fresh, freshOptions, 7);
    PumpUntil([&] { return !service.View(start).pending && startQueries >= 2; }, "a new Start-only opening queries current state");
    const auto state = freshOptions.pollItems(fresh, true);
    Expect(state && freshOptions.pollItemsFinished() && ordinaryQueries == previousOrdinary,
        "enabling only Start pinning never triggers an aggregate query on reopening");
    service.Prewarm(first);
    auto prewarmed = first; prewarmed.startPinOnly = true;
    PumpUntil([&] { return service.View(prewarmed).snapshot.has_value(); }, "Start-only prewarming uses its dedicated request");
    Expect(ordinaryQueries == previousOrdinary, "Start-only selection prewarming does not enumerate unrelated extensions");
}

void TestUnchangedCataloguePersistence()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto cachePath = directory.path / L"cache";
    std::filesystem::create_directory(cachePath);
    const auto cataloguePath = cachePath / L"catalogue.bin";
    ext::Catalogue original; original.revision = 17;
    ext::Registration row; row.id = "reg:unchanged"; row.revision = 17;
    row.contexts = ext::ContextBit(ext::Context::File); row.types = {L"*"};
    row.verbs = {"unchanged"}; row.display.label = L"Original";
    row.linked = true; original.rows = {row};
    original.associations.push_back({"verb:unchanged", row.id, ext::Context::File});
    const auto bytes = snowdesktop::settings_ipc::Pack(std::uint32_t(4), original);
    {
        std::ofstream out(cataloguePath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    const auto savedTime = std::filesystem::last_write_time(cataloguePath) - std::chrono::hours(1);
    std::filesystem::last_write_time(cataloguePath, savedTime);
    std::atomic<unsigned> scans = 0, queries = 0;
    std::atomic<std::uint64_t> revision = 17;
    ext::MenuService service(cachePath, [&](const auto &) {
        ++queries;
        return ext::QueryWork{[] { return ext::Reply{true, {}, {}}; }, {}};
    }, [&] {
        auto value = original;
        value.associations.clear(); value.rows[0].linked = false;
        value.revision = value.rows[0].revision = revision.load();
        if (value.revision != 17) value.rows[0].display.label = L"Changed";
        ++scans; return value;
    });
    service.Inspect();
    PumpUntil([&] { return scans.load() != 0 && !service.Inspect().scanning; },
        "initial unchanged catalogue scan and discovery complete");
    Expect(std::filesystem::last_write_time(cataloguePath) == savedTime,
        "unchanged registry inventory does not rewrite the durable catalogue");
    const auto initialQueries = queries.load();
    const auto initialScans = scans.load();
    service.Inspect({}, true);
    PumpUntil([&] { return scans.load() > initialScans && !service.Inspect().scanning; },
        "explicit unchanged refresh still completes its scan");
    Expect(queries.load() > initialQueries,
        "unchanged catalogue fast path retains forced command discovery");
    Expect(std::filesystem::last_write_time(cataloguePath) == savedTime,
        "explicit unchanged refresh avoids redundant catalogue serialization");
    revision = 18;
    const auto previousScans = scans.load();
    service.Inspect({}, true);
    PumpUntil([&] { return scans.load() > previousScans && !service.Inspect().scanning &&
        std::filesystem::last_write_time(cataloguePath) != savedTime; },
        "changed registration revision replaces the durable catalogue");
    service.Shutdown();
    std::ifstream input(cataloguePath, std::ios::binary | std::ios::ate);
    const auto length = input.tellg(); input.seekg(0);
    snowdesktop::settings_ipc::Bytes updated(static_cast<size_t>(length));
    Expect(static_cast<bool>(input.read(reinterpret_cast<char*>(updated.data()), updated.size())),
        "read refreshed catalogue");
    const auto [schema, value] = snowdesktop::settings_ipc::Unpack<std::tuple<std::uint32_t, ext::Catalogue>>(updated);
    Expect(schema == 4 && value.revision == 18 && value.rows[0].display.label == L"Changed",
        "changed catalogue preserves schema and publishes new registration metadata");
    Expect(value.rows[0].linked && value.associations.size() == 1,
        "unchanged scans retain associations for subsequent changed registration scans");
}

void TestIdleCatalogueInvalidation()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto path = L"Software\\Classes\\Local Settings\\SnowDesktopCpuTest-" + directory.path.filename().wstring();
    struct Fixture
    {
        HKEY key = nullptr; std::wstring path; bool owned = false;
        ~Fixture() { if (key) RegCloseKey(key); if (owned) RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
    } fixture{nullptr, path};
    DWORD disposition = 0;
    Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
        nullptr, &fixture.key, &disposition) == ERROR_SUCCESS, "create isolated Classes notification fixture");
    fixture.owned = disposition == REG_CREATED_NEW_KEY;
    Expect(fixture.owned, "Classes notification fixture is newly owned by this test");
    std::atomic<unsigned> scans = 0, version = 1;
    ext::MenuService service(directory.path / L"cache", [&](const auto &) {
        ext::Reply reply; reply.ok = true;
        ext::Entry entry; entry.key = "cpu-test"; entry.provider = "verb:cpu-test";
        entry.label = std::to_wstring(version.load()); reply.entries = {entry};
        return ext::QueryWork{[reply] { return reply; }, {}};
    }, [&] {
        ext::Catalogue result; result.revision = version.load();
        ext::Registration row; row.id = "reg:cpu-test"; row.contexts = 1;
        row.types = {L"*"}; row.verbs = {"cpu-test"}; row.revision = result.revision;
        row.display.label = std::to_wstring(result.revision); result.rows = {row};
        ++scans; return result;
    });
    ext::Preferences prefs; ext::SetCommon(prefs, "reg:cpu-test", ext::Category::Objects, true);
    service.Configure(prefs);
    const auto target = directory.path / L"selected.cpu-test"; std::ofstream(target) << "fixture";
    ext::Request request; request.paths = {target.wstring()};
    service.Query(request);
    PumpUntil([&] { return scans > 0 && service.View(request).snapshot.has_value(); }, "seed a current catalogue and menu");
    service.Inspect();
    PumpUntil([&] { return !service.Inspect().scanning; }, "initial settings discovery settles before registry notifications");
    const auto baseline = scans.load();
    auto notify = [&] {
        const DWORD data = ++version;
        Expect(RegSetValueExW(fixture.key, L"Sequence", 0, REG_DWORD,
            reinterpret_cast<const BYTE *>(&data), sizeof(data)) == ERROR_SUCCESS, "emit real registry value notification");
    };
    notify();
    PumpUntil([&] { return !service.View(request).snapshot.has_value(); }, "a registry notification retires the old menu snapshot");
    Sleep(100);
    Expect(scans == baseline, "an idle registry notification does not start a complete catalogue scan");
    service.Query(request);
    PumpUntil([&] { return scans > baseline && service.View(request).snapshot &&
        service.View(request).snapshot->entries.front().label == L"2"; }, "the next menu query refreshes the stale catalogue and command snapshot");
    const auto afterQuery = scans.load();
    notify();
    PumpUntil([&] { return !service.View(request).snapshot.has_value(); }, "a subsequent notification still retires the current snapshot");
    Expect(scans == afterQuery, "a subsequent idle notification remains lazy");
    service.Inspect();
    PumpUntil([&] { return scans > afterQuery && !service.Inspect().scanning; }, "settings inspection refreshes the stale catalogue without an explicit refresh button");
    service.Shutdown();
    const auto file = directory.path / L"cache" / L"catalogue.bin";
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    const auto length = input.tellg(); Expect(length > 0, "refreshed catalogue was persisted"); input.seekg(0);
    snowdesktop::settings_ipc::Bytes bytes(static_cast<size_t>(length));
    Expect(static_cast<bool>(input.read(reinterpret_cast<char *>(bytes.data()), bytes.size())), "read refreshed persisted catalogue");
    const auto [schema, catalogue] = snowdesktop::settings_ipc::Unpack<std::tuple<std::uint32_t, ext::Catalogue>>(bytes);
    Expect(schema == 4 && catalogue.revision == 3 && catalogue.rows.front().display.label == L"3",
        "lazy refresh still publishes and persists changed registration metadata");
}

void TestCatalogueShutdown()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    std::promise<void> entered, release, shutdownEntered;
    auto readerEntered = entered.get_future();
    auto releaseReader = release.get_future().share();
    auto stopping = shutdownEntered.get_future();
    std::atomic_bool readerCompleted = false, readerTimedOut = false;
    ext::MenuService service(directory.path / L"cache", [](const auto &) {
        return ext::QueryWork{[] { return ext::Reply{true, {}, {}}; }, {}};
    }, [&] {
        entered.set_value();
        readerTimedOut = releaseReader.wait_for(std::chrono::seconds(10)) !=
            std::future_status::ready;
        readerCompleted = true;
        ext::Catalogue catalogue;
        catalogue.revision = 1;
        return catalogue;
    });
    // Only the registry reader is gated: scheduling and shutdown are real.
    // A scan that survives Shutdown can access destroyed process-wide caches
    // during a version switch and trigger the old host's crash restart.
    service.Inspect({}, true);
    const bool started = readerEntered.wait_for(std::chrono::seconds(10)) ==
        std::future_status::ready;
    if (!started)
    {
        release.set_value();
        service.Shutdown();
        Expect(false, "catalogue reader starts before the shutdown probe");
    }
    auto shutdown = std::async(std::launch::async, [&] {
        shutdownEntered.set_value();
        service.Shutdown();
        return readerCompleted.load();
    });
    const bool stoppingStarted = stopping.wait_for(std::chrono::seconds(10)) ==
        std::future_status::ready;
    const bool returnedEarly = shutdown.wait_for(std::chrono::milliseconds(250)) ==
        std::future_status::ready;
    // Release the reader and join before any assertion can unwind its state.
    release.set_value();
    const bool stopped = shutdown.wait_for(std::chrono::seconds(10)) ==
        std::future_status::ready;
    const bool drained = shutdown.get();
    service.Shutdown(); // Repeated explicit shutdown and destruction are safe.
    Expect(stoppingStarted && stopped && !readerTimedOut,
        "controlled shutdown completes without reader or worker timeout");
    Expect(!returnedEarly && drained,
        "Shutdown must drain the catalogue reader before host teardown can destroy its caches");
}

void TestSnapshotPresentation()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    const auto path = directory.path / L"presentation.txt";
    { std::ofstream file(path); file << "private target"; }
    ext::Request request; request.paths = {path.wstring()};
    ext::MenuService service(directory.path / L"cache");
    ext::Preferences prefs;
    menu::Item more; more.label = L"More"; more.command = 7;
    {
        ext::Presentation hidden(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        hidden.Attach(items, options, 7);
        Expect(items.size() == 1 && !options.pollItems && !service.View(request).pending, "default-hidden popup starts no Shell query");
    }
    ext::SetHidden(prefs, "verb:first", ext::Context::File, false);
    {
        ext::Presentation cold(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        cold.Attach(items, options, 7);
        Expect(items.size() == 1 && options.pollItems, "cold popup starts with base commands and an asynchronous completion hook, without a placeholder");
        PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "first uncached popup completes its real query");
        Expect(!options.pollItems(items, false), "cold completion waits while the pointer or cascade prevents safe insertion");
        const auto loaded = options.pollItems(items, true);
        Expect(loaded && loaded->size() == 2 && loaded->front().label == L"压缩" && loaded->back().command == 7,
               "first uncached popup inserts enabled commands above More when the renderer permits it");
    }
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "closing a popup leaves the host query alive to warm the next popup");
    const auto first = service.View(request);
    {
        ext::Presentation warm(request, prefs, L"", L"", service);
        std::vector<menu::Item> items = {more}; menu::Options options;
        warm.Attach(items, options, 7);
        Expect(items.size() == 2 && items.front().label == L"压缩" && items.back().command == 7 && !options.pollItems,
               "warm popup reads immutable memory snapshot above the bottom footer");
        service.Query(request, ext::QueryPriority::Menu, true);
        PumpUntil([&] { return service.View(request).revision > first.revision; }, "background refresh finishes");
        Expect(items.size() == 2 && !options.pollItems, "completed refresh cannot alter the open menu or its hit regions");
    }
    const auto otherPath = directory.path / L"quick-close.txt";
    { std::ofstream file(otherPath); file << "private target"; }
    auto other = request; other.paths = {otherPath.wstring()};
    { ext::Presentation closed(other, prefs, L"", L"", service); }
    PumpUntil([&] { return service.View(other).snapshot.has_value(); }, "closing an uncached popup keeps its shared query alive");
}
void TestPendingCachedClick()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory directory;
    const auto path = directory.path / L"retry.txt", output = directory.path / L"invoked.txt";
    { std::ofstream file(path); file << "private target"; }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE", output.c_str());
    ext::Request request; request.paths = {path.wstring()};
    ext::Reply cached; cached.ok = true;
    ext::Entry entry; entry.label = L"Cached command"; entry.key = "cached"; entry.provider = "verb:cached"; entry.token = 999;
    cached.entries = {entry};
    const auto cachePath = directory.path / L"cache";
    ext::MenuSnapshotCache cache(cachePath);
    Expect(cache.Store(cache.Capture(request), cached), "seed a private disk snapshot");
    {
        ext::MenuService service(cachePath);
        PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "startup restores the exact selection from disk on the service worker");
        ext::Preferences prefs; ext::SetHidden(prefs, "verb:cached", ext::Context::File, false);
        ext::Presentation presentation(request, prefs, L"", L"", service);
        std::vector<menu::Item> items; menu::Options options; presentation.Attach(items, options, 0);
        Expect(items.size() == 1, "disk-restored snapshot is immediately available to popup");
        service.Query(request, ext::QueryPriority::Menu, true);
        PumpUntil([&] { return !service.View(request).error.empty(); }, "controlled failed refresh completes");
        Expect(service.View(request).snapshot.has_value() && !options.pollItems, "failed query preserves both snapshot and frozen popup");
        Expect(presentation.Invoke(items.front().command, {0, 0}), "explicit cached click bypasses automatic backoff once");
        PumpUntil([&] { return std::filesystem::exists(output); }, "click reaches the supervised child's real invocation transport");
        UINT token = 0; { std::ifstream file(output); file >> token; }
        Expect(token == 72, "execute only the fresh session's token, never cached token 999");
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE", nullptr);
}

void TestInvocationOwnerHandoff()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto file = directory.path / L"owner.txt"; std::ofstream(file) << "isolated";
    const HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Invocation owner",
        WS_POPUP, -12000, -12000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Expect(owner != nullptr, "create a private persistent invocation owner");
    const auto captured = snowdesktop::ShellInvocationOwner::Capture(owner);
    Expect(captured.Resolve() == owner, "real top-level owner survives an asynchronous command handoff");
    auto stale = captured; ++stale.thread;
    Expect(!stale.Resolve(), "owner thread mismatch rejects a stale invocation identity");
    ext::Request request; request.paths = {file.wstring()};
    ext::Reply reply; reply.ok = true;
    ext::Entry command; command.key = "test-owner"; command.provider = "verb:test-owner";
    command.label = L"Private owner handoff"; command.token = 42; reply.entries = {command};
    std::atomic<HWND> received = nullptr;
    std::atomic<bool> done = false;
    ext::MenuService service(directory.path / L"cache", [&, reply](const auto &) {
        return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {
            Expect(false, "a real owner must reach the owner-aware execution boundary");
        }, [&](UINT token, POINT point, HWND window) {
            Expect(token == 42 && point.x == 12 && point.y == 34, "handoff preserves the fresh command and click position");
            received = window;
        }};
    }, [] { return ext::Catalogue{}; });
    service.Execute(request, ext::AppendReference({}, command), {12, 34}, [&](bool ok) { done = ok; }, owner);
    PumpUntil([&] { return done.load(); }, "the real service completes an owner-aware click");
    Expect(received == owner, "cached/fresh-query scheduling preserves the host owner instead of a hidden query window");
    DestroyWindow(owner);
    Expect(!captured.Resolve(), "a destroyed menu owner is rejected before invocation");
}
void TestSourceScheduler()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto file = directory.path / L"source.snowattribution"; std::ofstream(file) << "private";
    ext::Request request; request.paths = {file.wstring()};
    ext::Registration handler; handler.id = "clsid:{00000000-0000-0000-0000-000000000001}";
    handler.kind = ext::RegistrationKind::Handler; handler.contexts = 1; handler.types = {L".snowattribution"};
    handler.sources = {L"Test.Type\\shellex\\ContextMenuHandlers\\Provider"};
    handler.verbs = {"{00000000-0000-0000-0000-000000000001}"}; handler.application = {"test-app", L"Test app"};
    handler.revision = 1;
    ext::Catalogue catalogue; catalogue.revision = 1; catalogue.rows = {handler};
    ext::Entry action; action.provider = "verb:runtime-only"; action.key = "runtime-only"; action.label = L"Runtime action"; action.token = 72;
    std::atomic<int> probes = 0, invokes = 0, active = 0, maximum = 0;
    std::atomic<bool> release = false;
    auto factory = [&](const ext::Request &target) -> ext::QueryWork {
        auto lease = std::shared_ptr<int>(new int, [&](int *p) { --active; delete p; });
        const auto count = ++active; maximum.store(std::max(maximum.load(), count));
        const bool source = !target.sourceClsid.empty();
        if (source) ++probes;
        ext::Reply reply; reply.ok = true;
        if (target.paths == request.paths) reply.entries = {action};
        if (source) { auto hidden = action; hidden.key = "probe-only"; hidden.provider = "verb:probe-only"; reply.entries.push_back(hidden); }
        return {[&, lease, reply, source]() -> std::optional<ext::Reply> { if (source && !release) return {}; return reply; },
            [&, source](UINT token, POINT) { Expect(!source && token == 72, "source sessions cannot become execution sessions"); ++invokes; }};
    };
    const auto cache = directory.path / L"cache";
    {
        ext::MenuService service(cache, factory, [catalogue] { return catalogue; });
        service.Inspect(request);
        PumpUntil([&] { return probes > 0; }, "unattributed actual item schedules a separate provider query");
        const auto first = service.View(request);
        Expect(first.snapshot && first.snapshot->entries.size() == 1 && !first.pending, "slow source discovery does not delay or add items to the actual menu");
        bool completed = false;
        service.Execute(request, ext::AppendReference({}, action), {}, [&](bool ok) { completed = ok; });
        PumpUntil([&] { return completed; }, "explicit command executes while a metadata probe is held");
        Expect(maximum <= 2 && invokes == 1, "metadata work shares the two-worker limit and never invokes a command");
        // The first proof targets the previous snapshot revision; it must be
        // rejected after the execution requery, not applied to a newer menu.
        release = true;
        PumpUntil([&] { return !service.Inspect(request).scanning; }, "source discovery finishes without polling restarting the query");
        service.Inspect(request, true);
        PumpUntil([&] {
            const auto view = service.Inspect(request);
            return !view.scanning && std::any_of(view.catalogue.rows.begin(), view.catalogue.rows.end(), [&](const auto &item) {
                return item.id == action.provider && item.application == handler.application;
            });
        }, "fresh source proof updates the existing action metadata");
        const auto ready = service.Inspect(request);
        Expect(std::none_of(ready.catalogue.rows.begin(), ready.catalogue.rows.end(), [](const auto &item) { return item.id == "verb:probe-only"; }), "probe-only commands never enter the management catalogue");
        Expect(service.View(request).snapshot->entries.size() == 1, "attribution leaves menu content unchanged");
    }
    {
        ext::MenuService restored(cache, [](const auto &) -> ext::QueryWork { throw std::runtime_error("unavailable"); }, [catalogue] { return catalogue; });
        PumpUntil([&] {
            const auto view = restored.Inspect(request);
            return std::any_of(view.catalogue.rows.begin(), view.catalogue.rows.end(), [&](const auto &item) { return item.id == action.provider && item.application == handler.application; });
        }, "application proof survives restart when the next query cannot launch");
    }
}
void TestSourceDeduplication()
{
    // Repeating a common action on another file used to query every provider
    // again, keeping first-run discovery busy for minutes.
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    ext::Registration handler; handler.id = "clsid:dedup"; handler.kind = ext::RegistrationKind::Handler;
    handler.contexts = 1; handler.types = {L".snowattribution"}; handler.revision = 1;
    handler.sources = {L"Test.Type\\shellex\\ContextMenuHandlers\\Provider"};
    handler.verbs = {"{00000000-0000-0000-0000-000000000001}"}; handler.application = {"test-app", L"Test app"};
    ext::Catalogue catalogue; catalogue.revision = 1; catalogue.rows = {handler};
    std::atomic<int> probes = 0;
    auto factory = [&](const ext::Request &target) -> ext::QueryWork {
        ext::Reply reply; reply.ok = true;
        if (!target.sourceClsid.empty())
        {
            ++probes;
            if (probes > 1) { reply.ok = false; reply.error = "controlled source failure"; }
        }
        else if (!target.paths.empty() && std::filesystem::path(target.paths.front()).extension() == L".snowattribution")
        {
            const auto name = std::filesystem::path(target.paths.front()).stem().string();
            ext::Entry item; item.provider = "verb:" + (name == "a" || name == "b" ? std::string("common") : name);
            item.key = item.provider; item.label = L"Actual action"; reply.entries = {item};
        }
        return {[reply]() -> std::optional<ext::Reply> { return reply; }, {}};
    };
    ext::MenuService service(directory.path / L"cache", factory, [catalogue] { return catalogue; });
    auto inspect = [&](const wchar_t *name, bool refresh = false) {
        const auto file = directory.path / name; std::ofstream(file) << "private";
        ext::Request request; request.paths = {file.wstring()}; service.Inspect(request, refresh);
        PumpUntil([&] { const auto view = service.Inspect(request); return service.View(request).snapshot.has_value() && !view.scanning && !view.menu.pending; },
            "source discovery finishes for the actual requested file");
    };
    inspect(L"a.snowattribution"); Expect(probes == 1, "first unknown action probes its registered source once");
    inspect(L"b.snowattribution"); Expect(probes == 1, "same action on another file reuses the completed attribution attempt");
    inspect(L"c.snowattribution"); Expect(probes == 2, "a new actual action permits another source check");
    inspect(L"d.snowattribution"); Expect(probes == 2, "failed source is not relaunched for every remaining file");
    inspect(L"d.snowattribution", true); Expect(probes > 2, "explicit refresh permits a failed source to be checked again");
}
void TestQueryScheduler()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    struct Gate { std::atomic<bool> ready = false; ext::Reply reply; };
    std::mutex mutex;
    std::vector<std::shared_ptr<Gate>> gates;
    std::atomic<int> active = 0, maximum = 0, starts = 0, invokes = 0;
    std::atomic<bool> failStart = false;
    auto factory = [&](const ext::Request &) -> ext::QueryWork {
        ++starts;
        if (failStart) throw std::runtime_error("controlled startup failure");
        auto gate = std::make_shared<Gate>();
        auto lease = std::shared_ptr<int>(new int, [&](int *p) { --active; delete p; });
        const auto count = ++active; maximum.store(std::max(maximum.load(), count));
        gate->reply.ok = true; ext::Entry entry; entry.provider = "verb:test"; entry.key = "test"; entry.label = L"Test"; entry.token = 42;
        gate->reply.entries = {entry};
        { std::lock_guard lock(mutex); gates.push_back(gate); }
        return {[gate, lease]() -> std::optional<ext::Reply> { if (!gate->ready) return {}; return gate->reply; }, [&](UINT token, POINT) { if (token == 42) ++invokes; }};
    };
    ext::MenuService service(directory.path / L"cache", factory, [] { ext::Catalogue c; c.revision = 1; return c; });
    const auto path = directory.path / L"a.txt"; { std::ofstream f(path); f << "a"; }
    ext::Request request; request.paths = {path.wstring()};
    service.Query(request); service.Query(request, ext::QueryPriority::Inspect);
    PumpUntil([&] { return starts == 1; }, "one real selection creates one shared task");
    std::shared_ptr<Gate> first;
    { std::lock_guard lock(mutex); first = gates.front(); }
    first->ready = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "publish complete successful query");
    const auto snapshot = service.View(request).snapshot;
    Expect(snapshot->entries.front().token == 0, "published snapshots never contain executable tokens");
    failStart = true;
    service.Query(request, ext::QueryPriority::Menu, true);
    PumpUntil([&] { return !service.View(request).error.empty(); }, "controlled launch failure reaches the scheduler");
    Expect(service.View(request).snapshot.has_value(), "launch failure does not clear a valid memory snapshot");
    const int failedStarts = starts;
    for (int i = 0; i < 20; ++i) service.Query(request);
    Expect(starts == failedStarts && !service.View(request).pending, "automatic failures back off without spawning repeated helpers");
    failStart = false;
    service.Query(request, ext::QueryPriority::Menu, true);
    PumpUntil([&] { return starts > failedStarts; }, "explicit refresh can bypass backoff");
    std::shared_ptr<Gate> stale;
    { std::lock_guard lock(mutex); stale = gates.back(); }
    service.Invalidate(request);
    stale->reply.entries.front().label = L"stale"; stale->ready = true;
    PumpUntil([&] { return starts > failedStarts + 1; }, "invalidated completion schedules a newer dependency query");
    Expect(!service.View(request).snapshot, "late result from an old dependency cannot restore invalidated menu");
    std::shared_ptr<Gate> newest;
    { std::lock_guard lock(mutex); newest = gates.back(); }
    newest->reply.entries.clear(); newest->ready = true;
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "a successful empty result is publishable");
    Expect(service.View(request).snapshot->entries.empty(), "valid empty menu replaces an older nonempty menu");
    // Hold two distinct requests and verify the third remains queued.
    std::vector<ext::Request> requests;
    for (int i = 0; i < 3; ++i)
    {
        auto r = request; r.paths = {(directory.path / (std::to_wstring(i) + L".txt")).wstring()};
        { std::ofstream f(r.paths.front()); f << i; } requests.push_back(r); service.Query(r);
    }
    PumpUntil([&] { std::lock_guard lock(mutex); return std::count_if(gates.begin(), gates.end(), [](const auto &g) { return !g->ready; }) == 2; }, "two queries run concurrently");
    {
        std::lock_guard lock(mutex);
        for (auto &gate : gates) gate->ready = true;
    }
    PumpUntil([&] { return service.View(requests[0]).snapshot && service.View(requests[1]).snapshot; }, "completed workers drain the queue");
    PumpUntil([&] { std::lock_guard lock(mutex); for (auto &gate : gates) gate->ready = true; return service.View(requests[2]).snapshot.has_value(); }, "queued third query completes");
    std::atomic<bool> rejected = false;
    ext::Entry clicked; clicked.provider = "verb:test"; clicked.key = "test"; clicked.label = L"Test";
    service.Execute(requests[2], ext::AppendReference({}, clicked), {}, [&](bool ok) { rejected = !ok; });
    PumpUntil([&] { std::lock_guard lock(mutex); return !gates.back()->ready; }, "explicit click starts a fresh command query");
    service.Configure({}); // User hides extensions while the execution query is pending.
    { std::lock_guard lock(mutex); gates.back()->ready = true; }
    PumpUntil([&] { return rejected.load(); }, "latest visibility rules reject a pending click after disable");
    Expect(invokes == 0, "disabled pending command is never invoked");
    service.Shutdown();
    Expect(maximum <= 2, "scheduler never runs more than two query workers");
}

void TestMenuPromotesQueuedPrewarm()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    ext::Request blocker, selected, later;
    blocker.paths = {(temp.path / L"blocker.txt").wstring()};
    // Equal-length ordered names make equal due ticks deterministic as well.
    selected.paths = {(temp.path / L"aa-popup.txt").wstring()};
    later.paths = {(temp.path / L"zz-popup.txt").wstring()};
    for (const auto &request : {blocker, selected, later})
        std::ofstream(std::filesystem::path(request.paths.front())) << "private scheduler target";
    std::atomic<bool> entered = false;
    bool release = false;
    std::mutex gateMutex;
    std::condition_variable gate;
    std::mutex mutex;
    std::vector<ext::Request> launched;
    ext::Reply reply; reply.ok = true;
    ext::Entry entry; entry.provider = "verb:inspect"; entry.key = "inspect"; entry.label = L"Inspect";
    reply.entries = {entry};
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &request) {
        if (request.paths == blocker.paths)
        {
            entered = true;
            std::unique_lock lock(gateMutex);
            if (!gate.wait_for(lock, std::chrono::seconds(5), [&] { return release; }))
                throw std::runtime_error("scheduler gate timed out");
        }
        { std::lock_guard lock(mutex); launched.push_back(request); }
        return ext::QueryWork{[reply] { return reply; }, {}};
    }, [] { return ext::Catalogue{}; });
    ext::Preferences prefs; ext::SetCommon(prefs, "verb:inspect", ext::Category::Objects, true);
    service.Configure(prefs);
    service.Query(blocker, ext::QueryPriority::Inspect, true);
    PumpUntil([&] { return entered.load(); }, "hold the process boundary while queuing selection and popup requests");
    service.Prewarm(selected);
    service.Query(selected);
    service.Query(later);
    { std::lock_guard lock(gateMutex); release = true; }
    gate.notify_one();
    PumpUntil([&] { return service.View(selected).snapshot && service.View(later).snapshot &&
        !service.View(selected).pending && !service.View(later).pending; }, "both interactive menus complete through the scheduler");
    std::lock_guard lock(mutex);
    Expect(launched.size() == 3 && launched[1].paths == selected.paths && launched[2].paths == later.paths,
        "a popup promotes its queued prewarm ahead of a later popup and queries that selection exactly once");
}

void TestBackgroundQueriesReserveMenuSlot()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    ext::Request first, second, interactive, start;
    first.paths = {(temp.path / L"first.txt").wstring()};
    second.paths = {(temp.path / L"second.txt").wstring()};
    interactive.paths = {(temp.path / L"interactive.txt").wstring()};
    start.paths = {(temp.path / L"start.lnk").wstring()}; start.startPinOnly = true;
    for (const auto &request : {first, second, interactive, start})
        std::ofstream(std::filesystem::path(request.paths.front())) << "private scheduler target";
    std::atomic<bool> release = false;
    std::mutex mutex;
    std::vector<ext::Request> launched;
    ext::Reply reply; reply.ok = true;
    ext::Entry entry; entry.provider = "verb:inspect"; entry.key = "inspect"; entry.label = L"Inspect";
    reply.entries = {entry};
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &request) {
        if (!request.startPinOnly) { std::lock_guard lock(mutex); launched.push_back(request); }
        return ext::QueryWork{[&, reply, background = request.paths == first.paths || request.paths == second.paths]() -> std::optional<ext::Reply> {
            if (background && !release) return {};
            return reply;
        }, {}};
    }, [] { return ext::Catalogue{}; });
    ext::Preferences prefs;
    ext::SetCommon(prefs, "verb:inspect", ext::Category::Objects, true);
    ext::SetCommon(prefs, "state:start-pin", ext::Category::Objects, true);
    service.Configure(prefs);
    service.Query(first, ext::QueryPriority::Inspect, true);
    PumpUntil([&] { std::lock_guard lock(mutex); return launched.size() == 1; }, "first background query occupies the shared worker lane");
    service.Query(second, ext::QueryPriority::Inspect, true);
    service.Query(start);
    // A completed reserved-lane query proves that the worker has processed the
    // dispatch loop. No latency threshold or sleep is used to infer absence.
    PumpUntil([&] { const auto view = service.View(start); return view.snapshot && !view.pending; },
        "reserved Start query provides a causal scheduler barrier");
    service.Query(interactive);
    PumpUntil([&] { const auto view = service.View(interactive);
        std::lock_guard lock(mutex);
        return (view.snapshot && !view.pending) || launched.size() >= 2; }, "observe which request received the second arbitrary-extension slot");
    bool reserved = false;
    { std::lock_guard lock(mutex); reserved = launched.size() == 2 && launched[1].paths == interactive.paths; }
    release = true;
    PumpUntil([&] { const auto view = service.View(second); return view.snapshot && !view.pending; }, "queued background discovery resumes after interactive work");
    Expect(reserved, "background inspection cannot occupy both workers and delay an explicit popup");
}

void TestVisibilityScheduling()
{
    // Exercise production configuration, popup projection and scheduler. Only
    // the child process boundary is replaced; no desktop interaction is used.
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory temp;
    const auto file = temp.path / L"cached.txt", folder = temp.path / L"folder";
    std::ofstream(file) << "private"; std::filesystem::create_directory(folder);
    ext::Request request; request.paths = {file.wstring()};
    ext::Reply reply; reply.ok = true;
    for (const auto *id : {"a", "b"})
    {
        ext::Entry entry; entry.provider = id; entry.key = id; entry.label = std::wstring(1, static_cast<wchar_t>(*id));
        entry.token = 71; reply.entries.push_back(entry);
    }
    const auto cachePath = temp.path / L"cache";
    ext::MenuSnapshotCache cache(cachePath);
    Expect(cache.Store(cache.Capture(request), reply, ext::MenuSnapshotCache::Now() - 60000), "seed an older valid display snapshot");
    std::mutex mutex;
    std::vector<ext::Request> launched;
    auto factory = [&](const ext::Request &target) {
        { std::lock_guard lock(mutex); launched.push_back(target); }
        return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {}};
    };
    ext::MenuService service(cachePath, factory, [] { ext::Catalogue c; c.revision = 1; return c; });
    service.Configure({});
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "restore raw cached commands while all switches are off");
    ext::Preferences prefs; ext::SetCommon(prefs, "a", ext::Category::Objects, true);
    service.Configure(prefs);
    ext::Preferences stale; ext::SetCommon(stale, "b", ext::Category::Objects, true);
    for (int i = 0; i < 30; ++i)
    {
        ext::Presentation popup(request, stale, L"", L"", service);
        std::vector<menu::Item> items; menu::Options options; popup.Attach(items, options, 0);
        Expect(items.size() == 1 && items[0].label == L"a" && !options.pollItems, "every cache hit uses current host visibility rather than captured popup settings");
    }
    service.Configure(stale);
    ext::Presentation changed(request, prefs, L"", L"", service);
    std::vector<menu::Item> changedItems; menu::Options changedOptions; changed.Attach(changedItems, changedOptions, 0);
    Expect(changedItems.size() == 1 && changedItems[0].label == L"b", "switch edits immediately change cached menu projection");
    Expect(service.View(request).snapshot->entries.size() == 2, "visibility edits preserve reusable unfiltered content");
    service.Configure({});
    service.Query(request); service.Prewarm(request);
    Expect(!service.MenuEnabled(request, prefs) && service.MenuDisplay(request, prefs).snapshot->entries.empty(), "all-off host rules override stale enabled popup settings");
    // An Inspect barrier runs after any accidentally queued Menu work, giving
    // the no-query assertion a deterministic observation point.
    auto barrier = request; barrier.paths = {folder.wstring()};
    service.Query(barrier, ext::QueryPriority::Inspect, true);
    PumpUntil([&] { return service.View(barrier).snapshot.has_value(); }, "explicit management inspection remains available with all switches off");
    { std::lock_guard lock(mutex); Expect(launched.size() == 1 && launched[0].paths == barrier.paths, "thirty cached opens and disabled prewarm never launch a helper"); }
    prefs.shown.push_back({"legacy", ext::Context::File});
    ext::SetCommon(prefs, "legacy", ext::Category::Objects, false);
    ext::SetOverride(prefs, "a", ext::Context::File, ext::Visibility::Hide);
    ext::SetOverride(prefs, "a", ext::Context::Folder, ext::Visibility::Hide);
    Expect(!ext::HasOptIns(prefs), "false common rules and explicit hides suppress retained legacy opt-ins");
    ext::SetOverride(prefs, "a", ext::Context::File, ext::Visibility::Show);
    service.Configure(prefs);
    auto mixed = request; mixed.paths.push_back(folder.wstring());
    service.Query(mixed);
    auto background = request; background.paths = {folder.wstring()}; background.background = true; background.context = ext::Context::FolderBackground;
    service.Query(background);
    service.Query(barrier, ext::QueryPriority::Inspect, true);
    PumpUntil([&] { return !service.View(barrier).pending && !service.View(mixed).pending; }, "resolve object scopes before dispatching a mixed selection");
    { std::lock_guard lock(mutex); Expect(launched.size() == 2, "mixed-location hide and unrelated background scope skip Shell enumeration"); }
    // A changed local target must still invalidate a valid display snapshot.
    std::ofstream(file, std::ios::app) << "changed target";
    service.Query(request);
    PumpUntil([&] { std::lock_guard lock(mutex); return launched.size() == 3 && !service.View(request).pending; }, "target changes still trigger a background requery");
    service.Configure({});
    std::ofstream(file, std::ios::app) << "changed during management";
    service.Query(request, ext::QueryPriority::Inspect);
    PumpUntil([&] { std::lock_guard lock(mutex); return launched.size() == 4 && service.View(request).snapshot.has_value() && !service.View(request).pending; }, "an explicit cached inspection can requery changed objects with all switches off");
}

void TestDisabledQueuedQueries()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    std::atomic<bool> release = false;
    std::atomic<int> starts = 0;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &) {
        ++starts;
        return ext::QueryWork{[&]() -> std::optional<ext::Reply> {
            if (!release) return {}; ext::Reply r; r.ok = true; return r;
        }, {}};
    }, [] { ext::Catalogue c; c.revision = 1; return c; });
    ext::Preferences prefs; ext::SetCommon(prefs, "a", ext::Category::Objects, true); service.Configure(prefs);
    std::vector<ext::Request> requests;
    for (int i = 0; i < 4; ++i)
    {
        const auto path = temp.path / (std::to_wstring(i) + L".txt"); std::ofstream(path) << "fixture";
        ext::Request r; r.paths = {path.wstring()}; requests.push_back(r); service.Query(r);
    }
    PumpUntil([&] { return starts == 2; }, "hold two supervised queries before disabling queued work");
    service.Query(requests[3], ext::QueryPriority::Inspect);
    service.Configure({});
    Expect(!service.View(requests[2]).pending && service.View(requests[3]).pending, "disable cancels queued popup work while preserving a joined inspection");
    release = true;
    PumpUntil([&] { return service.View(requests[3]).snapshot.has_value(); }, "explicit inspection drains after disable");
    Expect(starts == 3 && !service.View(requests[2]).snapshot, "cancelled popup request never reaches the child process boundary");
    service.Configure(prefs); release = false;
    service.Query(requests[0], ext::QueryPriority::Menu, true);
    service.Query(requests[1], ext::QueryPriority::Menu, true);
    PumpUntil([&] { return starts == 5; }, "hold both slots while a prewarm gains an inspection subscriber");
    service.Prewarm(requests[2]); service.Query(requests[2], ext::QueryPriority::Inspect);
    const auto nextFile = temp.path / L"next.txt"; std::ofstream(nextFile) << "fixture";
    ext::Request next; next.paths = {nextFile.wstring()}; service.Prewarm(next);
    Expect(service.View(requests[2]).pending, "changing selection cannot cancel a prewarm shared with explicit inspection");
    service.Configure({}); release = true;
    PumpUntil([&] { return service.View(requests[2]).snapshot.has_value(); }, "shared inspection completes after ordinary prewarm is disabled");
    Expect(starts == 6 && !service.View(next).snapshot, "only the subscribed inspection survives the replacement prewarm");
}

void TestKnownScopeQueryPolicy()
{
    namespace ext = snowdesktop::shell_extensions;
    namespace menu = snowdesktop::modern_menu;
    TemporaryDirectory temp;
    ext::Request image, text;
    image.paths = {(temp.path / L"image.png").wstring()}; text.paths = {(temp.path / L"document.txt").wstring()};
    std::ofstream(image.paths[0]) << "private"; std::ofstream(text.paths[0]) << "private";
    ext::Catalogue catalogue; catalogue.revision = 1;
    ext::Registration registration; registration.id = "image-action"; registration.contexts = 1;
    registration.types = {L".png"}; registration.verbs = {"image"}; registration.revision = 1;
    catalogue.rows.push_back(registration);
    registration.id = "system-disabled"; registration.systemEnabled = false; registration.types = {L".txt"};
    catalogue.rows.push_back(registration);
    std::atomic<int> starts = 0;
    std::atomic<bool> ready = false;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &) {
        ++starts;
        return ext::QueryWork{[&]() -> std::optional<ext::Reply> {
            if (!ready) return {};
            ext::Reply reply; reply.ok = true; ext::Entry e;
            e.provider = "verb:image"; e.key = "image"; e.label = L"Image command"; reply.entries = {e}; return reply;
        }, {}};
    }, [catalogue] { return catalogue; });
    ext::Preferences prefs;
    ext::SetCommon(prefs, "image-action", ext::Category::Objects, true);
    ext::SetCommon(prefs, "system-disabled", ext::Category::Objects, true);
    service.Configure(prefs);
    PumpUntil([&] { return !service.MenuEnabled(text, prefs); }, "background catalogue excludes wrong-type and system-disabled registrations");
    service.Query(text); service.Prewarm(text);
    ext::Presentation popup(image, prefs, L"", L"", service);
    std::vector<menu::Item> items; menu::Options options; popup.Attach(items, options, 0);
    PumpUntil([&] { return starts == 1; }, "eligible first uncached popup queries dynamically");
    Expect(bool(options.pollItems) && items.empty(), "uncached query starts without a loading placeholder");
    service.Configure({}); ready = true;
    PumpUntil([&] { return service.View(image).snapshot.has_value(); }, "valid in-flight result can warm cache after disable");
    const auto filled = options.pollItems(items, true);
    Expect(filled && filled->empty() && starts == 1, "first-load completion uses new hidden settings and never queries unrelated text handlers");
    service.Configure(prefs);
    Expect(service.MenuDisplay(image, {}).snapshot->entries.size() == 1, "reenabling applies immediately to the raw warmed cache");
}

void TestSelectionScopes()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto a = temp.path / L"a.txt", b = temp.path / L"b.txt", c = temp.path / L"c.png";
    const auto d = temp.path / L"folder1", e = temp.path / L"folder2";
    for (const auto &path : {a, b, c}) { std::ofstream out(path); out << "fixture"; }
    std::filesystem::create_directory(d); std::filesystem::create_directory(e);
    const std::vector<std::vector<std::wstring>> selections = {
        {a.wstring()}, {a.wstring(), b.wstring()}, {a.wstring(), c.wstring()},
        {d.wstring()}, {d.wstring(), e.wstring()}, {a.wstring(), d.wstring()}};
    const unsigned expectedContexts[] = {1, 1, 1, 2, 2, 3};
    std::vector<ext::Request> requests;
    for (bool shift : {false, true}) for (const auto &paths : selections)
    { ext::Request request; request.paths = paths; request.extended = shift; requests.push_back(request); }
    std::atomic<int> starts = 0;
    ext::MenuService service(temp.path / L"cache", [&](const ext::Request &request) {
        ++starts;
        const auto found = std::find_if(requests.begin(), requests.end(), [&](const auto &r) { return r.paths == request.paths && r.extended == request.extended; });
        Expect(found != requests.end(), "query boundary receives the complete selection and Shift state");
        ext::Reply reply; reply.ok = true; ext::Entry entry; entry.provider = "provider"; entry.key = "action";
        entry.label = std::to_wstring(std::distance(requests.begin(), found)); entry.token = 17; reply.entries = {entry};
        return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {}};
    });
    for (const auto &request : requests) service.Query(request);
    PumpUntil([&] { return std::all_of(requests.begin(), requests.end(), [&](const auto &r) { return service.View(r).snapshot.has_value(); }); }, "all single and mixed selections publish independently");
    ext::Preferences preferences; ext::SetCommon(preferences, "provider", ext::Category::Objects, true);
    ext::SetOverride(preferences, "provider", ext::Context::Folder, ext::Visibility::Hide);
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const auto view = service.View(requests[i]);
        Expect(view.contexts == expectedContexts[i % 6] && view.snapshot->entries.front().label == std::to_wstring(i), "different selections and Shift never borrow a snapshot");
        Expect(ext::VisibleSnapshot(preferences, *view.snapshot, view.contexts).empty() == (i % 6 >= 3), "folder hiding wins for folder-only and mixed file-folder selections");
        service.Query(requests[i], ext::QueryPriority::Inspect);
    }
    Expect(starts == 12, "fresh settings and popup access reuse all twelve exact snapshots");
}

void TestCatalogueDependencies()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    const auto path = temp.path / L"a.txt"; { std::ofstream out(path); out << "fixture"; }
    ext::Request request; request.paths = {path.wstring()};
    std::atomic<int> scanNumber = 0, mode = 0;
    auto reader = [&] {
        ext::Catalogue catalogue; catalogue.revision = 10 + mode.load();
        ext::Registration text; text.id = "text"; text.contexts = 1; text.types = {L".txt"}; text.revision = mode == 2 ? 2 : 1;
        ext::Registration image; image.id = "image"; image.contexts = 1; image.types = {L".png"}; image.revision = mode >= 1 ? 2 : 1;
        catalogue.rows = {text, image}; ++scanNumber; return catalogue;
    };
    ext::MenuService service(temp.path / L"cache", [](const auto &) {
        ext::Reply reply; reply.ok = true; return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) {}};
    }, reader);
    service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 1 && !service.Inspect().scanning; }, "initial dependency catalogue is ready");
    service.Query(request);
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "seed an exact text selection");
    service.Inspect(request);
    const auto revision = service.View(request).revision;
    mode = 1; service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 2 && !service.Inspect().scanning; }, "unrelated registration scan completes");
    Expect(service.View(request).snapshot && service.View(request).revision == revision, "an image-only registry change retains the text snapshot");
    mode = 2; service.Inspect({}, true);
    PumpUntil([&] { return scanNumber >= 3 && !service.Inspect().scanning; }, "relevant registration scan completes");
    Expect(!service.View(request).snapshot, "a relevant registration change invalidates its dependent snapshot");
    Expect(service.Inspect(request).menu.pending, "a relevant dependency invalidation still refreshes the inspected selection");
    PumpUntil([&] { return service.View(request).snapshot.has_value(); }, "the invalidated current selection publishes a fresh snapshot");
}

void TestUsefulManagementItems()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    std::atomic<bool> release = false;
    std::atomic<int> queries = 0;
    auto registry = [] {
        ext::Catalogue catalogue; catalogue.revision = 1;
        for (int i = 0; i < 3000; ++i)
        {
            ext::Registration row; row.id = "reg:unused-" + std::to_string(i); row.contexts = 15;
            row.display.label = L"Duplicate registry label"; catalogue.rows.push_back(row);
        }
        return catalogue;
    };
    ext::MenuService service(temp.path / L"cache", [&](const auto &) {
        ++queries;
        return ext::QueryWork{[&]() -> std::optional<ext::Reply> {
            if (!release) return {};
            ext::Reply reply; reply.ok = true;
            ext::Entry first; first.provider = "menu:archive:one"; first.label = L"Archive";
            first.width = first.height = 1; first.pixels = {20, 40, 60, 255};
            ext::Entry child; child.label = L"Compress"; child.key = "compress"; child.token = 17;
            first.children = {child};
            auto second = first; second.provider = "menu:archive:two";
            auto disabled = first; disabled.provider = "disabled"; disabled.enabled = false;
            auto placeholder = first; placeholder.provider = "placeholder"; placeholder.label = L"…";
            reply.entries = {first, second, disabled, placeholder};
            return reply;
        }, [](UINT, POINT) {}};
    }, registry);
    const auto cold = service.Inspect();
    Expect(cold.catalogue.rows.empty() && cold.scanning, "uncached settings return immediately and start background discovery");
    PumpUntil([&] { return queries >= 2; }, "uncached settings actively query real baseline objects without requiring opt-ins");
    const auto pending = service.Inspect();
    Expect(pending.catalogue.rows.empty(), "three thousand unassociated registry records never become disabled settings controls");
    release = true;
    PumpUntil([&] { const auto view = service.Inspect(); return !view.scanning; }, "settings dynamically receive useful Shell roots on first access");
    const auto loaded = service.Inspect();
    Expect(loaded.catalogue.rows.size() == 2, "only the two actionable observed identities reach settings");
    Expect(queries == 4, "default discovery is bounded to four real object/context queries");
    Expect(loaded.catalogue.rows[0].id != loaded.catalogue.rows[1].id, "same-name actual providers are not merged by label");
    for (const auto &row : loaded.catalogue.rows)
    {
        Expect(row.linked && row.systemEnabled && row.contexts == 15, "same identity merges observed scopes into an actionable switch");
        Expect(row.display.children.empty() && !row.display.token && row.display.pixels.size() == 4, "settings carry only root labels and icons, not every dynamic child or token");
        ext::Preferences prefs; ext::SetCommon(prefs, row.id, ext::Category::Objects, true);
        ext::Reply actual; actual.ok = true; actual.entries = {row.display};
        Expect(ext::VisibleSnapshot(prefs, actual, 3).size() == 1, "a displayed unassociated provider switch controls the production visibility path");
    }
    Expect(snowdesktop::settings_ipc::Pack(loaded).size() < 8192, "settings IPC payload does not scale with the raw registry inventory");
    std::cout << "Management projection: registry=3000, rows=" << loaded.catalogue.rows.size()
              << ", bytes=" << snowdesktop::settings_ipc::Pack(loaded).size() << '\n';

    service.Inspect({}, true);
    PumpUntil([&] { return !service.Inspect().scanning; }, "explicit refresh completes baseline discovery");
    Expect(queries == 8, "explicit settings refresh queries all four locations despite the fresh-cache throttle");

    ext::Request previous; previous.paths = {(temp.path / L"previous.txt").wstring()};
    service.Manage(previous);
    ext::Request desktop; desktop.context = ext::Context::Desktop; desktop.background = true;
    const auto switching = service.Inspect(desktop);
    Expect(switching.selection.context == ext::Context::Desktop,
           "desktop inspection never routes settings back to the previous file while resolving its path");
    PumpUntil([&] {
        const auto selected = service.Inspect(desktop).selection;
        return selected.context == ext::Context::Desktop && !selected.paths.empty();
    }, "desktop inspection resolves its real path in the background");
}

// Real service/registry association, persistence and discovery scheduling remain
// under test; only the external Shell boundary supplies deterministic commands.
void TestFileTypeDiscovery()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    std::atomic<bool> imageEnabled = true;
    auto registry = [&] {
        ext::Catalogue value; value.revision = imageEnabled ? 1 : 2;
        ext::Registration image; image.id = "reg:image"; image.contexts = 1;
        image.application = {"app:image", L"Image provider"};
        image.types = {L".png"}; image.verbs = {"image-action"}; image.revision = imageEnabled ? 1 : 2;
        image.systemEnabled = imageEnabled;
        ext::Registration document; document.id = "reg:document"; document.contexts = 1;
        document.types = {L".docx"}; document.verbs = {"document-action"}; document.revision = 1;
        auto disabled = document; disabled.id = "reg:disabled"; disabled.types = {L".pdf"}; disabled.systemEnabled = false;
        value.rows = {image, document, disabled}; return value;
    };
    std::mutex requestsMutex;
    std::vector<ext::Request> queried;
    auto query = [&](const ext::Request &request) {
        { std::lock_guard lock(requestsMutex); queried.push_back(request); }
        ext::Reply reply; reply.ok = true;
        ext::Entry common; common.provider = "menu:common"; common.label = L"Common extension";
        reply.entries = {common};
        const auto extension = std::filesystem::path(request.paths.front()).extension().wstring();
        if (extension == L".png" || extension == L".docx")
        {
            std::ifstream file(std::filesystem::path(request.paths.front()), std::ios::binary); char magic[4]{}; file.read(magic, 4);
            Expect(extension == L".png" ? static_cast<unsigned char>(magic[0]) == 0x89 && magic[1] == 'P' : magic[0] == 'P' && magic[1] == 'K',
                "automatic image/document inspection supplies actual format data, not renamed empty files");
            auto item = common;
            item.provider = extension == L".png" ? "menu:image" : "menu:document";
            item.key = extension == L".png" ? "image-action" : "document-action";
            item.label = extension == L".png" ? L"Image action" : L"Document action";
            item.token = 88;
            reply.entries.push_back(item);
            if (extension == L".png" && request.paths.size() == 2)
            { item.provider = "menu:image-batch"; item.key.clear(); item.label = L"Batch images"; reply.entries.push_back(item); }
            if (extension == L".png" && request.extended)
            { item.provider = "menu:image-shift"; item.key.clear(); item.label = L"Advanced image command"; reply.entries.push_back(item); }
        }
        return ext::QueryWork{[reply] { return reply; }, [](UINT, POINT) { Expect(false, "discovery never invokes an extension"); }};
    };
    const auto cachePath = temp.path / L"cache";
    ext::Request imageRequest;
    {
        ext::MenuService service(cachePath, query, registry);
        service.Inspect();
        PumpUntil([&] { return !service.Inspect().scanning; }, "type discovery completes without user-picked files or opt-ins");
        const auto available = service.Inspect().catalogue;
        const auto contains = [&](const char *id) { return std::any_of(available.rows.begin(), available.rows.end(), [&](const auto &r) { return r.id == id; }); };
        Expect(contains("reg:image") && contains("reg:document") && contains("menu:image-batch") && contains("menu:image-shift"),
            "settings discover type-specific, multi-select and Shift-only actionable entries");
        Expect(std::count_if(available.rows.begin(), available.rows.end(), [](const auto &r) { return r.id == "menu:common"; }) == 1,
            "a common extension across file types and selection shapes remains one settings switch");
        {
            std::lock_guard lock(requestsMutex);
            for (const auto *type : {L".png", L".docx"}) for (bool multi : {false, true}) for (bool shift : {false, true})
                Expect(std::any_of(queried.begin(), queried.end(), [&](const auto &r) {
                    return std::filesystem::path(r.paths.front()).extension() == type && r.paths.size() == (multi ? 2u : 1u) && r.extended == shift;
                }), "normal and Shift discovery cover both single and same-type multiple selections");
            Expect(std::none_of(queried.begin(), queried.end(), [](const auto &r) { return std::filesystem::path(r.paths.front()).extension() == L".pdf"; }),
                "system-disabled type registrations do not create discovery jobs");
            imageRequest = *std::find_if(queried.begin(), queried.end(), [](const auto &r) { return std::filesystem::path(r.paths.front()).extension() == L".png"; });
        }
        service.Inspect(imageRequest); // Start once; subsequent settings polls are read-only.
        std::vector<ext::Request> extra;
        for (int i = 0; i < 70; ++i)
        {
            const auto file = temp.path / (std::to_wstring(i) + L".txt"); std::ofstream(file) << "fixture";
            ext::Request request; request.paths = {file.wstring()}; extra.push_back(request); service.Query(request);
        }
        PumpUntil([&] { return std::all_of(extra.begin(), extra.end(), [&](const auto &r) { return !service.View(r).pending; }) && !service.View(imageRequest).snapshot; },
            "exact image snapshot is evicted by later selections");
        Expect(!service.Inspect(imageRequest).menu.pending, "settings polling never requeues an already inspected selection after eviction");
        Expect(service.Inspect(imageRequest, true).menu.pending, "explicit refresh still queries the inspected selection after eviction");
        const auto retained = service.Inspect().catalogue.rows;
        Expect(std::any_of(retained.begin(), retained.end(), [](const auto &r) { return r.id == "reg:image"; }),
            "cache eviction never removes a discovered type-specific settings switch");
        service.Shutdown();
    }
    // Remove only exact snapshots in this test's private cache. The durable
    // management inventory must survive without them, even while Shell is down.
    for (const auto &file : std::filesystem::directory_iterator(cachePath))
        if (const auto name = file.path().filename().wstring(); (name.size() == 68 || name.size() == 70) && file.path().extension() == L".bin")
            std::filesystem::remove(file.path());
    {
        ext::MenuService restored(cachePath, [](const auto &) {
            return ext::QueryWork{[] { return ext::Reply{false, {}, "controlled offline helper"}; }, [](UINT, POINT) {}};
        }, registry);
        restored.Inspect();
        PumpUntil([&] { return !restored.Inspect().scanning; }, "restored management inventory remains available through failed fresh queries");
        auto items = restored.Inspect().catalogue.rows;
        Expect(std::any_of(items.begin(), items.end(), [](const auto &r) { return r.id == "reg:image"; }) &&
            std::any_of(items.begin(), items.end(), [](const auto &r) { return r.id == "reg:document"; }),
            "image and document switches restore independently of exact snapshots and helper success");
        Expect(std::any_of(items.begin(), items.end(), [](const auto &r) { return r.id == "reg:image" && r.application.id == "app:image" && r.application.name == L"Image provider"; }),
            "providing application metadata restores from the shared management cache while Shell is unavailable");
        for (const auto &item : items)
            Expect(!item.display.token && item.display.children.empty(), "durable inventory contains only root display metadata");
        imageEnabled = false;
        restored.Inspect({}, true);
        PumpUntil([&] { return !restored.Inspect().scanning; }, "system-disable refresh completes");
        items = restored.Inspect().catalogue.rows;
        Expect(std::none_of(items.begin(), items.end(), [](const auto &r) { return r.id == "reg:image" || r.id == "menu:image-batch" || r.id == "menu:image-shift"; }) &&
            std::any_of(items.begin(), items.end(), [](const auto &r) { return r.id == "reg:document"; }),
            "related system changes retire persisted image entries without dropping unrelated document entries");
    }
}

// Opt-in hardware evidence: uses the real production helper without displaying
// or operating the SnowDesktop desktop. Not part of portable automatic tests.
void ProbeNvidiaCompatibility(bool invoke)
{
    namespace ext = snowdesktop::shell_extensions;
    ext::Request request; request.context = ext::Context::Desktop; request.background = true;
    request.paths = {snowdesktop::DesktopShellInvocationDirectory()};
    ext::Session session(request);
    std::optional<ext::Reply> reply;
    PumpUntil([&] { if (!reply) reply = session.Poll(); return reply.has_value(); },
        "real NVIDIA desktop menu query completes");
    Expect(reply->ok, "real NVIDIA desktop menu query succeeds");
    const auto catalogue = ext::ReadCatalogue();
    auto linked = catalogue; ext::Associate(linked, request, *reply);
    const auto found = std::find_if(reply->entries.begin(), reply->entries.end(), [](const auto &entry) {
        return entry.registration == ext::NvidiaControlPanelRegistration;
    });
    Expect(found != reply->entries.end() && found->enabled && found->token,
        "the previously missing installed NVIDIA Control Panel is now an executable desktop menu item");
    Expect(std::count_if(reply->entries.begin(), reply->entries.end(), [](const auto &entry) {
        return entry.registration == ext::NvidiaControlPanelRegistration;
    }) == 1, "exactly one Control Panel entry is returned");
    Expect(!found->label.empty() && !found->pixels.empty(), "installed application supplies a title and icon");
    const auto settings = ext::ManagementRows(linked, ext::Category::Background);
    Expect(std::any_of(settings.begin(), settings.end(), [](const auto &row) {
        return std::any_of(row.members.begin(), row.members.end(), [](const auto &member) {
            return member.id == ext::NvidiaControlPanelRegistration;
        });
    }), "the actual compatibility entry is manageable under its NVIDIA registration");
    ext::Preferences prefs;
    ext::SetCommon(prefs, ext::NvidiaControlPanelRegistration, ext::Category::Background, true);
    auto visible = ext::VisibleSnapshot(prefs, *reply, ext::ContextBit(ext::Context::Desktop));
    Expect(std::any_of(visible.begin(), visible.end(), [](const auto &entry) {
        return entry.registration == ext::NvidiaControlPanelRegistration;
    }), "the actual entry is visible when enabled");
    ext::SetCommon(prefs, ext::NvidiaControlPanelRegistration, ext::Category::Background, false);
    visible = ext::VisibleSnapshot(prefs, *reply, ext::ContextBit(ext::Context::Desktop));
    Expect(std::none_of(visible.begin(), visible.end(), [](const auto &entry) {
        return entry.registration == ext::NvidiaControlPanelRegistration;
    }), "the actual entry is hidden when disabled");
    std::cout << "NVIDIA compatibility: token=" << found->token << " icon=" << found->width << 'x' << found->height
              << " registration=" << found->registration << " helper=" << session.ProcessId() << std::endl;
    if (invoke)
    {
        session.Invoke(found->token, {});
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, session.ProcessId());
        if (process) { WaitForSingleObject(process, 10000); CloseHandle(process); }
        std::cout << "NVIDIA application open command dispatched; verify the launched application separately.\n";
    }
}

void BenchmarkManagement()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory temp;
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    ext::MenuService service(temp.path / L"cache");
    const auto started = std::chrono::steady_clock::now();
    const auto cold = service.Inspect();
    std::cout << "management first_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
              << " rows=" << cold.catalogue.rows.size() << std::endl;
    const auto deadline = GetTickCount64() + 30000;
    ext::CatalogueView ready;
    do
    {
        ready = service.Inspect();
        if (!ready.scanning) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    } while (GetTickCount64() < deadline);
    Expect(!ready.scanning && !ready.catalogue.rows.empty(), "real settings discovery produces actionable rows");
    std::cout << "management complete_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
              << " rows=" << ready.catalogue.rows.size() << " bytes=" << snowdesktop::settings_ipc::Pack(ready).size() << std::endl;
    const auto grouped = ext::ManagementRows(ready.catalogue, ext::Category::Objects);
    std::cout << "management object_rows=" << grouped.size() << " merged_groups=" << std::count_if(grouped.begin(), grouped.end(), [](const auto &r) { return r.members.size() > 1; }) << std::endl;
    for (int i = 0; i < 30; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto view = service.Inspect();
        std::cout << "management warm_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                  << " rows=" << view.catalogue.rows.size() << std::endl;
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr);
}

// Opt-in measurements use the real Session path and private files only. They
// do not invoke extensions or add machine-dependent latency assertions.
void BenchmarkMenus()
{
    namespace ext = snowdesktop::shell_extensions;
    TemporaryDirectory directory;
    const auto file = directory.path / L"sample.txt";
    { std::ofstream output(file); output << "menu timing sample"; }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    ext::InvalidateMenuCache();
    for (const bool folder : {false, true})
    {
        ext::Request request;
        request.paths = {(folder ? directory.path : file).wstring()};
        for (int iteration = 0; iteration < 35; ++iteration)
        {
            if (iteration < 5) ext::Session::ReleaseIdleWorker();
            const auto start = GetTickCount64();
            const auto diskStart=std::chrono::steady_clock::now();
            ext::MenuSnapshotCache disk(ext::SharedMenuCache().Directory());
            const auto ticket=disk.Capture(request);
            const auto snapshot=disk.Find(ticket);
            const auto diskMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-diskStart).count();
            ext::Session session(request);
            const auto launched = GetTickCount64();
            std::optional<ext::Reply> reply;
            while (!reply && GetTickCount64() - start < 10000)
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                reply = session.Poll();
                if (!reply) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            const auto elapsed = GetTickCount64() - start;
            std::cout << "menu_benchmark scope=" << (folder ? "folder" : "file")
                      << " mode=" << (iteration < 5 ? "cold" : "warm")
                      << " iteration=" << iteration << " snapshot=" << bool(snapshot) << " disk_ms=" << diskMs
                      << " start_ms=" << launched - start
                      << " ready_ms=" << elapsed << " ok=" << (reply && reply->ok)
                      << " worker=" << session.ProcessId()
                      << " entries=" << (reply ? reply->entries.size() : 0) << std::endl;
            Expect(reply && reply->ok, "benchmark real menu query succeeds");
            Expect(disk.Store(ticket,*reply),"benchmark stores the fresh display snapshot");
        }
    }
    for (const bool folder : {false, true})
    {
        ext::Request request; request.paths = {(folder ? directory.path : file).wstring()};
        std::unique_ptr<ext::MenuService> service;
        for (int iteration = 0; iteration < 35; ++iteration)
        {
            if (iteration < 5)
                service = std::make_unique<ext::MenuService>(directory.path / (L"service-" + std::to_wstring(folder) + L"-" + std::to_wstring(iteration)));
            const auto start = std::chrono::steady_clock::now();
            const auto before = service->View(request);
            const auto firstMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            service->Query(request, ext::QueryPriority::Menu, true);
            PumpUntil([&] { const auto view = service->View(request); return !view.pending && view.snapshot && view.revision > before.revision; }, "shared service benchmark query completes");
            const auto queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto hot = std::chrono::steady_clock::now();
            const auto snapshot = service->View(request);
            const auto hitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hot).count();
            ext::Preferences visible;
            for (const auto &entry : snapshot.snapshot->entries)
                if (!entry.separator) ext::SetHidden(visible, entry.provider, folder ? ext::Context::Folder : ext::Context::File, false);
            const auto prepare = std::chrono::steady_clock::now();
            ext::Presentation popup(request, visible, L"", L"", *service);
            snowdesktop::modern_menu::Item more; more.command = 7; more.label = L"More";
            std::vector<snowdesktop::modern_menu::Item> items{more}; snowdesktop::modern_menu::Options options;
            popup.Attach(items, options, 7);
            const auto prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare).count();
            std::cout << "service_benchmark scope=" << (folder ? "folder" : "file")
                      << " mode=" << (iteration < 5 ? "cold" : "warm") << " iteration=" << iteration
                      << " first_ms=" << firstMs << " memory_ms=" << hitMs << " prepare_ms=" << prepareMs << " query_ms=" << queryMs
                      << " entries=" << snapshot.snapshot->entries.size() << std::endl;
        }
    }
    const auto catalogueStart = std::chrono::steady_clock::now();
    const auto catalogue = ext::ReadCatalogue();
    std::cout << "catalogue_benchmark rows=" << catalogue.rows.size()
              << " enabled=" << std::count_if(catalogue.rows.begin(), catalogue.rows.end(), [](const auto &r) { return r.systemEnabled; })
              << " ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - catalogueStart).count() << std::endl;
    // Measure the real scheduler/command resolver with a harmless boundary
    // replacement. No installed third-party action is executed by this probe.
    ext::Reply harmless; harmless.ok = true;
    ext::Entry command; command.provider = "benchmark"; command.key = "noop"; command.label = L"No-op"; command.token = 42;
    harmless.entries = {command};
    ext::MenuService clickService(directory.path / L"click-benchmark", [harmless](const auto &) {
        return ext::QueryWork{[harmless] { return harmless; }, [](UINT, POINT) {}};
    });
    ext::Request clickRequest; clickRequest.paths = {file.wstring()};
    for (int i = 0; i < 30; ++i)
    {
        std::atomic<bool> completed = false, succeeded = false;
        const auto start = std::chrono::steady_clock::now();
        clickService.Execute(clickRequest, ext::AppendReference({}, command), {}, [&](bool ok) { succeeded = ok; completed = true; });
        PumpUntil([&] { return completed.load(); }, "harmless click dispatch completes");
        Expect(succeeded, "harmless click resolves the current command");
        std::cout << "click_benchmark boundary=controlled iteration=" << i << " ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << std::endl;
    }
    SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", nullptr);
    ext::InvalidateMenuCache();
}

int wmain(int argc, wchar_t **argv)
{
    const bool archiveProbe = argc == 2 && std::wstring_view(argv[1]) == L"--probe-archive-submenus";
    const bool powerShellProbe = argc == 2 && std::wstring_view(argv[1]) == L"--probe-powershell-submenus";
    const bool shortcutProbe = argc == 2 && std::wstring_view(argv[1]) == L"--probe-shortcut-archives";
    const bool nvidiaProbe = argc == 2 && (std::wstring_view(argv[1]) == L"--probe-nvidia-menu" ||
        std::wstring_view(argv[1]) == L"--probe-nvidia-menu-invoke");
    if (nvidiaProbe || archiveProbe || powerShellProbe || shortcutProbe) SetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU", L"1");
    snowdesktop::shell_extensions::QueryExecutor query;
    wchar_t realMode[4]{};
    if(!GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_REAL_MENU",realMode,4)) query=[](const auto& request) {
        if (!request.paths.empty() && request.paths.front() == L"synthetic-hang") Sleep(INFINITE);
        if(request.paths.size()==3&&request.paths[0]==L"read-disk-cache")
        {
            snowdesktop::shell_extensions::MenuSnapshotCache cache(request.paths[1]);
            snowdesktop::shell_extensions::Request file;file.paths={request.paths[2]};
            return cache.Find(cache.Capture(file),101).value_or(snowdesktop::shell_extensions::Reply{});
        }
        snowdesktop::shell_extensions::Reply reply; reply.ok = true;
        snowdesktop::shell_extensions::Entry entry; entry.label=L"压缩";entry.checked=true;entry.enabled=false;
        entry.key=request.paths.front()==L"synthetic-second" ? "second" : "first";
        if (request.paths.front() == L"synthetic-stdio")
        {
            DWORD written = 0;
            const bool isolated = GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR &&
                                  GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR &&
                                  GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "x", 1, &written, nullptr);
            entry.key = isolated && written == 1 ? "isolated-stdio" : "inherited-stdio";
        }
        if (request.catalogueOnly) entry.label=request.paths.front();
        entry.provider = "verb:" + entry.key;
        reply.entries.push_back(entry);
        if(std::filesystem::path(request.paths.front()).filename()==L"presentation.txt")
        {
            entry.label = L"保持选中";
            entry.key = "stable";
            entry.provider = "verb:stable";
            reply.entries.push_back(entry);
        }
        return reply;
    };
    wchar_t invocationPath[32768]{};
    const bool record=GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_MENU_INVOKE",invocationPath,32768)!=0;
    snowdesktop::shell_extensions::InvokeExecutor invoke;
    if(record)
    {
        query = [](const auto &request) {
            const auto path = std::filesystem::path(request.paths.front());
            const auto failureMarker = path.parent_path() / L"retry-failed-once";
            // Failed helpers are deliberately discarded. Keep the one-shot
            // fault in this test's private directory so a new helper can retry.
            if (path.filename() == L"retry.txt" && !std::filesystem::exists(failureMarker))
            {
                std::ofstream marker(failureMarker);
                marker << "failed once";
                return snowdesktop::shell_extensions::Reply{false, {}, "transient query failure"};
            }
            snowdesktop::shell_extensions::Reply reply;
            reply.ok = true;
            snowdesktop::shell_extensions::Entry entry;
            entry.label = L"Cached command";
            entry.key = "cached";
            entry.provider = "verb:cached";
            entry.token = 72;
            reply.entries={entry};
            return reply;
        };
        invoke=[path=std::filesystem::path(invocationPath)](UINT token,POINT){std::ofstream file(path);file<<token;};
    }
    wchar_t startPinPath[32768]{};
    snowdesktop::shell_extensions::StartPinExecutor startPin;
    if (GetEnvironmentVariableW(L"SNOWDESKTOP_TEST_START_PIN", startPinPath, 32768))
        startPin = [output = std::filesystem::path(startPinPath)](
            snowdesktop::shell_start_pin::Action action, const std::wstring &path, HWND owner, POINT point) {
            DWORD process = 0; GetWindowThreadProcessId(owner, &process);
            const auto bytes = snowdesktop::settings_ipc::Pack(path, action,
                process == GetCurrentProcessId(), point.x, point.y);
            const auto temporary = std::filesystem::path(output.wstring() + L".tmp");
            { std::ofstream file(temporary, std::ios::binary);
              file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
            std::filesystem::rename(temporary, output);
            return E_ACCESSDENIED; // Deliberate final failure must still be handled.
        };
    if (const auto helper = snowdesktop::shell_extensions::TryRunHelper(
            std::move(query), std::move(invoke), std::move(startPin))) return *helper;

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    try
    {
        TemporaryDirectory cacheDirectory;
        snowdesktop::shell_extensions::SharedMenuCache()=snowdesktop::shell_extensions::MenuSnapshotCache(cacheDirectory.path/L"shared");
        if (shortcutProbe) ProbeShortcutArchives();
        else if (archiveProbe) ProbeArchiveSubmenus();
        else if (powerShellProbe) ProbePowerShellSubmenus();
        else if (nvidiaProbe) ProbeNvidiaCompatibility(std::wstring_view(argv[1]) == L"--probe-nvidia-menu-invoke");
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--benchmark-menu-settings") BenchmarkManagement();
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--benchmark-shell-menu") BenchmarkMenus();
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--test-start-pin-helper") TestExposedStartPinHelper();
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--test-state-commands")
        { TestPairedCommandVisibility(); TestPairedCommandRefresh(); TestStartQueryScheduling(); TestExposedStartPinHelper(); }
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--test-menu-query-policy")
        {
            TestUnchangedCataloguePersistence();
            TestIdleCatalogueInvalidation();
            TestBackgroundQueriesReserveMenuSlot();
            TestMenuPromotesQueuedPrewarm();
            TestVisibilityScheduling();
            TestDisabledQueuedQueries();
            TestKnownScopeQueryPolicy();
            TestRegistryCatalogue();
            TestNvidiaCompatibility();
        }
        else
        {
            TestUnchangedCataloguePersistence();
            TestIdleCatalogueInvalidation();
            TestCatalogueShutdown();
            RunTests();
            TestDeferredPopups();
            TestCatalogueCache();
            TestRegistryCatalogue();
            TestNvidiaCompatibility();
            TestManagementUpdates();
            TestManagementFilters();
            TestSourceAttribution();
            TestExtensionSessions();
            TestExposedStartPinHelper();
            TestPairedCommandVisibility();
            TestPairedCommandRefresh();
            TestStartQueryScheduling();
            TestSnapshotPresentation();
            TestPendingCachedClick();
            TestInvocationOwnerHandoff();
            TestQueryScheduler();
            TestVisibilityScheduling();
            TestDisabledQueuedQueries();
            TestKnownScopeQueryPolicy();
            TestSourceScheduler();
            TestSourceDeduplication();
            TestSelectionScopes();
            TestCatalogueDependencies();
            TestUsefulManagementItems();
            TestFileTypeDiscovery();
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    std::cout << "shell context-menu invocation tests passed\n";
    return 0;
}
