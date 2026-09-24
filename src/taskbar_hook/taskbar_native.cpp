#include "taskbar_native.h"
#include "taskbar_classic_surface.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <MinHook.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <cwchar>

namespace snowdesktop::taskbar_hook::native
{
namespace
{
struct AccentPolicy
{
    int state = 0;
    DWORD flags = 0, color = 0, animation = 0;
};
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
    ~WindowState() { if (owner) CloseHandle(owner); }
};
std::mutex windowsMutex;
std::unordered_map<HWND, std::shared_ptr<WindowState>> windows;

std::shared_ptr<WindowState> Find(HWND window)
{
    std::lock_guard lock(windowsMutex);
    const auto found = windows.find(window);
    return found == windows.end() ? nullptr : found->second;
}

HRESULT WINAPI SetDwmHook(HWND window, DWORD attribute, const void* data, DWORD size)
{
    if (attribute == DWMWA_CLOAK && data && size == sizeof(BOOL))
        if (auto state = Find(window))
        {
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
                        WaitForSingleObject(state->owner, 0) == WAIT_TIMEOUT;
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
    const bool suppress = ShouldSuppressTaskbar(snapshot, reinterpret_cast<std::uintptr_t>(window));
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
            AccentPolicy policy;
            // DirectComposition owns tint/gradient/border. Accent supplies only
            // the system material underneath, so opacity is not applied twice.
            policy.state = (style.style & kStyleGlassBackdrop) ?
                ((style.style & kStyleAcrylicBackdrop) ? 4 : 3) : 2;
            policy.flags = policy.state == 4 ? 0 : 2;
            policy.color = policy.state == 4 ? 0x01000000 : 0;
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
                policy.state = 3; policy.flags = 2; policy.color = 0;
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
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_SIZE || message == WM_DPICHANGED || message == WM_THEMECHANGED || message == WM_SETTINGCHANGE || message == WM_DWMCOMPOSITIONCHANGED)
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
