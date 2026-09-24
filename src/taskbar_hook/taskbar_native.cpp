#include "taskbar_native.h"
#include "taskbar_classic_surface.h"
#include "taskbar_classic_appearance.h"
#include "../taskbar_monitor.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <MinHook.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <cwchar>
#include <vector>

namespace snowdesktop::taskbar_hook::native
{
namespace
{
struct CompositionData { int attribute; void* data; SIZE_T size; };
using SetComposition = BOOL(WINAPI*)(HWND, CompositionData*);
using GetComposition = BOOL(WINAPI*)(HWND, CompositionData*);
using SetDwm = HRESULT(WINAPI*)(HWND, DWORD, const void*, DWORD);
SetComposition originalComposition = nullptr;
SetDwm originalDwm = nullptr;
GetComposition getComposition = nullptr;
constexpr UINT_PTR kSubclass = 0x5344544e;
constexpr UINT_PTR kTimer = 0x5344544e;

struct WindowState
{
    SharedState* mapping = nullptr;
    HANDLE owner = nullptr;
    DWORD ownerId = 0;
    bool classic = false;
    bool primary = false, ownsAutoHide = false, updatingAutoHide = false;
    AppBarMessage appBarMessage = nullptr;
    std::mutex mutex;
    bool cloaked = false, styled = false;
    BOOL restoreCloak = FALSE;
    AccentPolicy restoreAccent{}, accent{};
    bool haveRestoreAccent = false;
    TargetAppearance applied;
    ULONGLONG appearanceRetryTick = 0;
    RECT bounds{};
    ClassicSurface surface;
    HHOOK menuMouseHook = nullptr;
    bool menuLoop = false;
    ULONGLONG contextMenuUntil = 0;
    DWORD contextMenuThread = 0;
    HWND contextMenuPopup = nullptr;
    bool overflowWasVisible = false;
    std::vector<HWND> previousPopups;
    ~WindowState()
    {
        if (menuMouseHook) UnhookWindowsHookEx(menuMouseHook);
        if (owner) CloseHandle(owner);
    }
};
std::mutex windowsMutex;
std::unordered_map<HWND, std::shared_ptr<WindowState>> windows;

std::shared_ptr<WindowState> Find(HWND window)
{
    std::lock_guard lock(windowsMutex);
    const auto found = windows.find(window);
    return found == windows.end() ? nullptr : found->second;
}

bool IsTrayOrigin(HWND source, HWND taskbar)
{
    const HWND root = source ? GetAncestor(source, GA_ROOT) : nullptr;
    if (!root) return false;
    if (root == taskbar) return true;
    wchar_t name[96]{};
    GetClassNameW(root, name, static_cast<int>(std::size(name)));
    if (wcscmp(name, L"NotifyIconOverflowWindow") != 0 &&
        wcscmp(name, L"TopLevelWindowForOverflowXamlIsland") != 0) return false;
    DWORD sourceProcess = 0, taskbarProcess = 0;
    GetWindowThreadProcessId(root, &sourceProcess);
    GetWindowThreadProcessId(taskbar, &taskbarProcess);
    return sourceProcess && sourceProcess == taskbarProcess &&
        snowdesktop::taskbar_monitor::Resolve(root) == snowdesktop::taskbar_monitor::Resolve(taskbar);
}

bool ReadMenu(DWORD thread, GUITHREADINFO& info)
{
    info = {};
    info.cbSize = sizeof(info);
    return GetGUIThreadInfo(thread, &info) && info.hwndMenuOwner &&
        (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
}

bool IsVisibleMenuPopup(HWND window)
{
    if (!window || !IsWindowVisible(window)) return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!(style & WS_POPUP) || (style & WS_CAPTION) == WS_CAPTION ||
        !(extended & WS_EX_TOOLWINDOW) || (extended & WS_EX_TRANSPARENT)) return false;
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (_wcsicmp(name, L"tooltips_class32") == 0 ||
        wcsstr(name, L"ToolTip") || wcsstr(name, L"Tooltip")) return false;
    DWORD cloak = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloak, sizeof(cloak))) && !cloak;
}

std::vector<HWND> VisibleMenuPopups()
{
    std::vector<HWND> result;
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        if (IsVisibleMenuPopup(window))
            reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

void ArmContextMenu(const std::shared_ptr<WindowState>& state)
{
    auto existing = VisibleMenuPopups();
    std::lock_guard lock(state->mutex);
    state->previousPopups = std::move(existing);
    state->contextMenuPopup = nullptr;
    state->contextMenuThread = 0;
    state->contextMenuUntil = GetTickCount64() + 1500;
}

bool HasContextMenu(HWND taskbar, const std::shared_ptr<WindowState>& state)
{
    GUITHREADINFO info{};
    const bool threadMenu = ReadMenu(GetWindowThreadProcessId(taskbar, nullptr), info);
    const HWND threadMenuOwner = threadMenu ? info.hwndMenuOwner : nullptr;
    const bool ownedMenu = threadMenu && IsTrayOrigin(threadMenuOwner, taskbar);
    bool overflowVisible = false;
    for (const auto* name : {L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland"})
    {
        const HWND overflow = FindWindowW(name, nullptr);
        DWORD cloak = 0;
        if (overflow && IsWindowVisible(overflow) && IsTrayOrigin(overflow, taskbar) &&
            SUCCEEDED(DwmGetWindowAttribute(overflow, DWMWA_CLOAKED, &cloak, sizeof(cloak))) && !cloak)
            overflowVisible = true;
    }
    const ULONGLONG now = GetTickCount64();
    std::lock_guard lock(state->mutex);
    if (overflowVisible && !state->overflowWasVisible && now >= state->contextMenuUntil)
        state->previousPopups = VisibleMenuPopups();
    state->overflowWasVisible = overflowVisible;
    if (overflowVisible) state->contextMenuUntil = now + 1500;
    if (state->menuLoop || ownedMenu) return true;
    if (state->contextMenuPopup)
    {
        if (IsVisibleMenuPopup(state->contextMenuPopup)) return true;
        state->contextMenuPopup = nullptr;
        state->contextMenuUntil = 0;
    }
    if (state->contextMenuThread)
    {
        if (ReadMenu(state->contextMenuThread, info)) return true;
        state->contextMenuThread = 0;
        state->contextMenuUntil = 0;
    }
    if (now < state->contextMenuUntil)
    {
        if (threadMenu || ReadMenu(0, info))
            state->contextMenuThread = GetWindowThreadProcessId(
                threadMenu ? threadMenuOwner : info.hwndMenuOwner, nullptr);
        else
        {
            // WPF/WinUI and SnowDesktop menus can use a nonactivating tool
            // popup without entering a Win32 menu loop. Only accept a new
            // popup following an actual tray interaction, on this monitor,
            // belonging to Explorer, the foreground app or our host.
            DWORD foregroundProcess = 0, taskbarProcess = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
            GetWindowThreadProcessId(taskbar, &taskbarProcess);
            for (const HWND popup : VisibleMenuPopups())
            {
                if (std::find(state->previousPopups.begin(), state->previousPopups.end(), popup) !=
                    state->previousPopups.end()) continue;
                DWORD process = 0;
                GetWindowThreadProcessId(popup, &process);
                if ((process == foregroundProcess || process == taskbarProcess || process == state->ownerId) &&
                    snowdesktop::taskbar_monitor::Resolve(popup) == snowdesktop::taskbar_monitor::Resolve(taskbar))
                {
                    state->contextMenuPopup = popup;
                    break;
                }
            }
        }
        return true;
    }
    return overflowVisible;
}

LRESULT CALLBACK MenuMouseProc(int code, WPARAM message, LPARAM data) try
{
    if (code == HC_ACTION && data && (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
        message == WM_NCRBUTTONDOWN || message == WM_NCRBUTTONUP))
    {
        const HWND source = reinterpret_cast<const MOUSEHOOKSTRUCT*>(data)->hwnd;
        // Observe the actual Explorer-thread recipient before a tray app opens
        // a menu owned by its own (possibly hidden) foreground window.
        std::vector<std::pair<HWND, std::shared_ptr<WindowState>>> targets;
        { std::lock_guard lock(windowsMutex);
          for (const auto& entry : windows) targets.push_back(entry); }
        for (const auto& [window, state] : targets)
            if (IsTrayOrigin(source, window))
            {
                ArmContextMenu(state);
                PostMessageW(window, RegisterWindowMessageW(kApplyMessageName), 0, 0);
            }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
catch (...)
{
    return CallNextHookEx(nullptr, code, message, data);
}

HRESULT WINAPI SetDwmHook(HWND window, DWORD attribute, const void* data, DWORD size)
{
    if (attribute == DWMWA_CLOAK && data && size == sizeof(BOOL))
        if (auto state = Find(window))
        {
            const bool contextMenu = HasContextMenu(window, state);
            bool enforce = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->cloaked)
                {
                    state->restoreCloak = *static_cast<const BOOL*>(data);
                    Snapshot snapshot;
                    // Observe a newly opened panel before the posted apply
                    // message arrives, so Explorer's own reveal is not blocked.
                    enforce = (ReadSharedSnapshot(state->mapping, snapshot)
                            ? ShouldSuppressTaskbar(snapshot, reinterpret_cast<std::uintptr_t>(window))
                            : state->mapping->enabled && state->mapping->suppressTaskbar) &&
                        WaitForSingleObject(state->owner, 0) == WAIT_TIMEOUT && !contextMenu;
                }
            }
            if (enforce)
            {
                const BOOL cloak = TRUE;
                return originalDwm(window, attribute, &cloak, sizeof(cloak));
            }
        }
    return originalDwm(window, attribute, data, size);
}

BOOL WINAPI SetCompositionHook(HWND window, CompositionData* data)
{
    if (data && data->attribute == 19 && data->data && data->size == sizeof(AccentPolicy))
        if (auto state = Find(window))
        {
            bool enforce = false;
            AccentPolicy policy;
            {
                std::lock_guard lock(state->mutex);
                if (state->styled)
                {
                    state->restoreAccent = *static_cast<const AccentPolicy*>(data->data);
                    state->haveRestoreAccent = true;
                    policy = state->accent;
                    enforce = state->mapping->enabled && state->mapping->appearanceEnabled &&
                        WaitForSingleObject(state->owner, 0) == WAIT_TIMEOUT;
                }
            }
            if (enforce)
            {
                CompositionData replacement{19, &policy, sizeof(policy)};
                return originalComposition(window, &replacement);
            }
        }
    return originalComposition(window, data);
}

bool InstallHooks(bool classic)
{
    // Hook/trampoline code remains mapped for Explorer's lifetime, including
    // calls already in flight when the host dies. Never disable other hooks.
    static std::once_flag dwmOnce, compositionOnce;
    static bool dwmReady = false, compositionReady = false;
    std::call_once(dwmOnce, [] {
        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
        void* entry = reinterpret_cast<void*>(&DwmSetWindowAttribute);
        if (MH_CreateHook(entry, reinterpret_cast<void*>(&SetDwmHook),
                reinterpret_cast<void**>(&originalDwm)) != MH_OK) return;
        dwmReady = MH_EnableHook(entry) == MH_OK;
    });
    if (!dwmReady) return false;
    if (classic) std::call_once(compositionOnce, [] {
        const auto user = GetModuleHandleW(L"user32.dll");
        void* entry = reinterpret_cast<void*>(GetProcAddress(user, "SetWindowCompositionAttribute"));
        getComposition = reinterpret_cast<GetComposition>(GetProcAddress(user, "GetWindowCompositionAttribute"));
        if (!entry || MH_CreateHook(entry, reinterpret_cast<void*>(&SetCompositionHook),
                reinterpret_cast<void**>(&originalComposition)) != MH_OK) return;
        compositionReady = MH_EnableHook(entry) == MH_OK;
    });
    return !classic || compositionReady;
}

TargetAppearance Resolve(HWND window, const Snapshot& snapshot)
{
    for (LONG index = 0; index < snapshot.targetCount; ++index)
        if (snapshot.targets[index].taskbar == reinterpret_cast<std::uintptr_t>(window))
            return snapshot.targets[index];
    TargetAppearance result;
    result.enabled = snapshot.defaultEnabled;
    result.style = snapshot.style;
    result.red = snapshot.red; result.green = snapshot.green; result.blue = snapshot.blue;
    result.alpha = snapshot.alpha; result.borderRed = snapshot.borderRed;
    result.borderGreen = snapshot.borderGreen; result.borderBlue = snapshot.borderBlue;
    result.borderAlpha = snapshot.borderAlpha; result.gradient = snapshot.gradient;
    return result;
}

void RestoreAppearance(HWND window, const std::shared_ptr<WindowState>& state)
{
    AccentPolicy accent;
    bool wasStyled, restoreAccent;
    {
        std::lock_guard lock(state->mutex);
        wasStyled = std::exchange(state->styled, false);
        accent = state->restoreAccent;
        restoreAccent = state->haveRestoreAccent;
    }
    state->surface.Reset();
    if (wasStyled)
    {
        if (restoreAccent)
        {
            CompositionData data{19, &accent, sizeof(accent)};
            originalComposition(window, &data);
        }
        // Ask Explorer to reconstruct its native material for the current
        // system theme, including changes made while our override was active.
        PostMessageW(window, WM_DWMCOMPOSITIONCHANGED, 0, 0);
        InvalidateRect(window, nullptr, TRUE);
    }
}

void Restore(HWND window, const std::shared_ptr<WindowState>& state)
{
    BOOL cloak;
    bool wasCloaked;
    {
        std::lock_guard lock(state->mutex);
        wasCloaked = std::exchange(state->cloaked, false);
        cloak = state->restoreCloak;
    }
    RestoreAppearance(window, state);
    if (wasCloaked) originalDwm(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

bool UpdateAutoHide(HWND window, const std::shared_ptr<WindowState>& state, bool requested)
{
    if (!state->primary || state->updatingAutoHide) return true;
    state->updatingAutoHide = true;
    struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset{state->updatingAutoHide};
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    const UINT_PTR current = state->appBarMessage(ABM_GETSTATE, &data);
    if (requested && !state->ownsAutoHide)
    {
        InterlockedCompareExchange(&state->mapping->autoHideRestore,
            (current & ABS_AUTOHIDE) ? TRUE : FALSE, -1);
        state->ownsAutoHide = true;
    }
    if (!state->ownsAutoHide) return true;
    const bool wanted = requested || state->mapping->autoHideRestore == TRUE;
    if (((current & ABS_AUTOHIDE) != 0) != wanted)
    {
        data.lParam = static_cast<LPARAM>(wanted ? current | ABS_AUTOHIDE :
            current & ~static_cast<UINT_PTR>(ABS_AUTOHIDE));
        state->appBarMessage(ABM_SETSTATE, &data);
    }
    // ABM_SETSTATE always returns TRUE; verify actual state independently.
    const bool applied = ((state->appBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0) == wanted;
    InterlockedExchange(&state->mapping->autoHideStatus,
        !applied ? kStatusFailed : requested ? kStatusApplied : kStatusIdle);
    if (applied && !requested)
    {
        state->ownsAutoHide = false;
        InterlockedExchange(&state->mapping->autoHideRestore, -1);
    }
    return applied;
}

LRESULT CALLBACK Subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

void Detach(HWND window, const std::shared_ptr<WindowState>& state, bool destroying = false)
{
    Restore(window, state);
    // If Shell has not accepted the restore yet, retain the owner-thread timer
    // to retry after normal shutdown or owner death. Never retain a dead HWND.
    if (!UpdateAutoHide(window, state, false) && !destroying) return;
    KillTimer(window, kTimer);
    RemoveWindowSubclass(window, Subclass, kSubclass);
    RemovePropW(window, kAttachedProperty);
    RemovePropW(window, kContextMenuProperty);
    std::lock_guard lock(windowsMutex);
    windows.erase(window);
}

bool Update(HWND window, const std::shared_ptr<WindowState>& state, bool force)
{
    if (state->updatingAutoHide) return true;
    if (WaitForSingleObject(state->owner, 0) != WAIT_TIMEOUT)
    {
        Detach(window, state);
        return false;
    }
    Snapshot snapshot;
    if (!ReadSharedSnapshot(state->mapping, snapshot)) return true;
    if (!snapshot.enabled || snapshot.ownerProcessId != state->ownerId)
    {
        Detach(window, state);
        return false;
    }
    UpdateAutoHide(window, state, snapshot.suppressTaskbar);
    const bool contextMenu = HasContextMenu(window, state);
    if (contextMenu) SetPropW(window, kContextMenuProperty, reinterpret_cast<HANDLE>(1));
    else RemovePropW(window, kContextMenuProperty);
    const bool suppress = ShouldSuppressTaskbar(snapshot, reinterpret_cast<std::uintptr_t>(window)) && !contextMenu;
    bool cloakChanged = false;
    BOOL restoreCloak = FALSE;
    {
        std::lock_guard lock(state->mutex);
        if (suppress != state->cloaked)
        {
            if (suppress)
            {
                DWORD previous = 0;
                if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &previous, sizeof(previous))))
                {
                    InterlockedExchange(&state->mapping->suppressionStatus, kStatusFailed);
                    return true;
                }
                state->restoreCloak = (previous & DWM_CLOAKED_APP) != 0;
            }
            restoreCloak = state->restoreCloak;
            state->cloaked = suppress;
            cloakChanged = true;
        }
    }
    if (cloakChanged)
    {
        const BOOL value = suppress ? TRUE : restoreCloak;
        const HRESULT result = originalDwm(window, DWMWA_CLOAK, &value, sizeof(value));
        InterlockedExchange(&state->mapping->suppressionStatus,
            FAILED(result) ? kStatusFailed : (snapshot.suppressTaskbar ? kStatusApplied : kStatusIdle));
        if (FAILED(result))
        {
            std::lock_guard lock(state->mutex);
            state->cloaked = !suppress;
        }
    }

    const auto style = Resolve(window, snapshot);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    const bool styled = state->classic && snapshot.appearanceEnabled && style.enabled &&
        !(contrast.dwFlags & HCF_HIGHCONTRASTON);
    bool wasStyled;
    { std::lock_guard lock(state->mutex); wasStyled = state->styled; }
    if (!styled)
    {
        state->appearanceRetryTick = 0;
        if (wasStyled) RestoreAppearance(window, state);
        if (state->classic) InterlockedExchange(&state->mapping->status, kStatusApplied);
    }
    if (styled && (!state->appearanceRetryTick || GetTickCount64() >= state->appearanceRetryTick ||
            !(style == state->applied)))
    {
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (force || !wasStyled || !(style == state->applied) || !EqualRect(&bounds, &state->bounds))
        {
            AccentPolicy policy = MakeClassicTaskbarPolicy(style);
            if (!wasStyled && getComposition)
            {
                AccentPolicy nativeAccent;
                CompositionData query{19, &nativeAccent, sizeof(nativeAccent)};
                if (getComposition(window, &query))
                { std::lock_guard lock(state->mutex);
                  state->restoreAccent = nativeAccent; state->haveRestoreAccent = true; }
            }
            { std::lock_guard lock(state->mutex); state->accent = policy; state->styled = true; }
            CompositionData data{19, &policy, sizeof(policy)};
            bool materialApplied = originalComposition(window, &data) != FALSE;
            if (!materialApplied && policy.state == 4)
            {
                policy = MakeClassicAccentPolicy(style, false);
                { std::lock_guard lock(state->mutex); state->accent = policy; }
                materialApplied = originalComposition(window, &data) != FALSE;
            }
            const HRESULT result = materialApplied ? state->surface.Draw(window, style) : E_FAIL;
            state->applied = style;
            state->bounds = bounds;
            InterlockedExchange(&state->mapping->status, FAILED(result) ? kStatusFailed : kStatusApplied);
            if (FAILED(result))
            {
                // A competing composition target or device failure must not
                // release suppression. Restore native material and retry with
                // a bounded cadence (or immediately after an appearance edit).
                state->appearanceRetryTick = GetTickCount64() + 10000;
                RestoreAppearance(window, state);
            }
            else state->appearanceRetryTick = 0;
        }
    }
    if (styled && FAILED(state->surface.Synchronize(window)))
        InterlockedExchange(&state->mapping->status, kStatusFailed);
    if (!snapshot.suppressTaskbar && !styled && !state->classic)
    { Detach(window, state); return false; }
    return true;
}

LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) try
{
    const auto state = Find(window);
    if (!state) return DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY)
    {
        Detach(window, state, true);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    const UINT apply = RegisterWindowMessageW(kApplyMessageName);
    if (message == apply)
    {
        Update(window, state, false);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    if (message == WM_TIMER && wParam == kTimer)
    { Update(window, state, false); return 0; }
    if (message == WM_ENTERMENULOOP || message == WM_CONTEXTMENU)
    {
        ArmContextMenu(state);
        { std::lock_guard lock(state->mutex);
          if (message == WM_ENTERMENULOOP) state->menuLoop = true; }
        // Release the owner before Explorer creates its menu; a later host
        // visibility scan can cloak the menu along with the taskbar on Win10.
        Update(window, state, false);
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_EXITMENULOOP)
    {
        { std::lock_guard lock(state->mutex);
          state->menuLoop = false; state->contextMenuUntil = 0; state->contextMenuThread = 0;
          state->contextMenuPopup = nullptr; state->previousPopups.clear(); }
        Update(window, state, false);
    }
    else if (message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW)
        Update(window, state, false);
    else if (message == WM_SIZE || message == WM_DPICHANGED || message == WM_THEMECHANGED || message == WM_SETTINGCHANGE || message == WM_DWMCOMPOSITIONCHANGED)
        Update(window, state, true);
    return result;
}
catch (...)
{
    if (auto state = Find(window))
    {
        InterlockedExchange(&state->mapping->status, kStatusFailed);
        InterlockedExchange(&state->mapping->suppressionStatus, kStatusFailed);
        Detach(window, state);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
}

bool IsClassicTaskbarPlatform() noexcept
{
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto getVersion = reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{sizeof(version)};
    return getVersion && getVersion(&version) == 0 && version.dwMajorVersion == 10 && version.dwBuildNumber < 22000;
}

void ObserveMenuMessage(HWND source, UINT message) noexcept try
{
    if (message != WM_ENTERMENULOOP && message != WM_EXITMENULOOP && message != WM_CONTEXTMENU)
        return;
    std::vector<std::pair<HWND, std::shared_ptr<WindowState>>> targets;
    { std::lock_guard lock(windowsMutex);
      for (const auto& entry : windows) targets.push_back(entry); }
    for (const auto& [window, state] : targets)
        if (IsTrayOrigin(source, window))
        {
            if (message != WM_EXITMENULOOP) ArmContextMenu(state);
            { std::lock_guard lock(state->mutex);
              if (message != WM_CONTEXTMENU) state->menuLoop = message == WM_ENTERMENULOOP;
              state->contextMenuThread = 0;
              state->contextMenuPopup = nullptr;
              state->contextMenuUntil = message == WM_EXITMENULOOP ? 0 : GetTickCount64() + 1500; }
            if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId())
                Update(window, state, false);
            else
                PostMessageW(window, RegisterWindowMessageW(kApplyMessageName), 0, 0);
        }
}
catch (...) {}

bool Attach(HWND window, SharedState* mapping, bool classic, AppBarMessage appBarMessage) try
{
    if (auto existing = Find(window)) return Update(window, existing, false);
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return false;
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"Shell_TrayWnd") != 0 && wcscmp(name, L"Shell_SecondaryTrayWnd") != 0) return false;
    Snapshot snapshot;
    if (!ReadSharedSnapshot(mapping, snapshot) || !snapshot.enabled ||
        (!classic && !snapshot.suppressTaskbar)) return false;
    auto state = std::make_shared<WindowState>();
    state->mapping = mapping; state->classic = classic; state->ownerId = snapshot.ownerProcessId;
    state->primary = wcscmp(name, L"Shell_TrayWnd") == 0;
    state->appBarMessage = appBarMessage;
    state->owner = OpenProcess(SYNCHRONIZE, FALSE, state->ownerId);
    if (!state->owner || WaitForSingleObject(state->owner, 0) != WAIT_TIMEOUT) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Attach), &pinned) || !InstallHooks(classic)) return false;
    if (!SetWindowSubclass(window, Subclass, kSubclass, 0)) return false;
    { std::lock_guard lock(windowsMutex); windows.emplace(window, state); }
    state->menuMouseHook = SetWindowsHookExW(WH_MOUSE, MenuMouseProc, nullptr, GetCurrentThreadId());
    if (!SetPropW(window, kAttachedProperty, reinterpret_cast<HANDLE>(1)) ||
        !SetTimer(window, kTimer, 250, nullptr))
    { Detach(window, state); return false; }
    return Update(window, state, true);
}
catch (...)
{
    if (auto state = Find(window)) Detach(window, state);
    return false;
}
}
