#include "dock_external_minimize.h"
#include "dock_minimize_protocol.h"
#include "dock_window_source_cloak.h"

#include <vector>
#include <utility>

bool DockExternalMinimize::Start(HWND receiver, const std::wstring& nativeDll,
    std::wstring helperExecutable, std::wstring helperDll)
{
    if (receiver_ == receiver && receiver_) return true;
    Stop();
    if (!receiver || !IsWindow(receiver) || !hooks_.Start(nativeDll)) return false;
    if (!SetPropW(receiver, snowdesktop::dock_minimize::kOwnerProperty,
            ULongToHandle(GetCurrentProcessId())))
    {
        hooks_.Stop();
        return false;
    }
    receiver_ = receiver;
    targetRevision_ = 1;
    SetPropW(receiver_, snowdesktop::dock_minimize::kRevisionProperty, ULongToHandle(targetRevision_));
    helperExecutable_ = std::move(helperExecutable);
    helperDll_ = std::move(helperDll);
    return true;
}

bool DockExternalMinimize::OwnsTarget(HWND window) const noexcept
{
    const auto found = targets_.find(window);
    DWORD process = 0;
    return found != targets_.end() && GetWindowThreadProcessId(window, &process) &&
        process == found->second &&
        GetPropW(window, snowdesktop::dock_minimize::kTargetProperty) == receiver_;
}

bool DockExternalMinimize::IsMinimizeButtonPress(HWND window, POINT screenPoint) const noexcept
{
    if (!OwnsTarget(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
    // The CBT request arrives on release. Classify the caption press before
    // the outside-pointer sampler can demote the Dock on button-down. Bound
    // each query so an unresponsive application cannot stall the desktop.
    constexpr UINT timeoutMs = 8;
    constexpr UINT flags = SMTO_ABORTIFHUNG | SMTO_BLOCK;
    DWORD_PTR hit = HTNOWHERE;
    if (!SendMessageTimeoutW(window, WM_NCHITTEST, 0,
            MAKELPARAM(screenPoint.x, screenPoint.y), flags, timeoutMs, &hit)) return false;
    if (static_cast<LRESULT>(hit) == HTMINBUTTON) return true;
    if (static_cast<LRESULT>(hit) != HTCLIENT && static_cast<LRESULT>(hit) != HTCAPTION) return false;
    // Custom frames can report HTCLIENT while retaining the system caption
    // button bounds. Use the actual minimize rectangle, never a guessed third
    // of the caption, so maximize/close and ordinary client clicks still dismiss.
    TITLEBARINFOEX title{sizeof(title)};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(window, WM_GETTITLEBARINFOEX, 0,
            reinterpret_cast<LPARAM>(&title), flags, timeoutMs, &result)) return false;
    constexpr DWORD unavailable = STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_UNAVAILABLE;
    return !(title.rgstate[2] & unavailable) &&
        !IsRectEmpty(&title.rgrect[2]) && PtInRect(&title.rgrect[2], screenPoint);
}

void DockExternalMinimize::UpdateTargets(std::span<const HWND> windows)
{
    if (!receiver_) return;
    bool changed = false;
    for (auto it = targets_.begin(); it != targets_.end();)
    {
        if (std::find(windows.begin(), windows.end(), it->first) == windows.end() ||
            !OwnsTarget(it->first))
        {
            if (OwnsTarget(it->first))
            {
                const auto owner = snowdesktop::dock_source_cloak::ReadOwner(it->first);
                if (owner.process == ULongToHandle(GetCurrentProcessId()))
                    snowdesktop::dock_source_cloak::Release(it->first, owner.window);
                RemovePropW(it->first, snowdesktop::dock_minimize::kTargetProperty);
            }
            it = targets_.erase(it);
            changed = true;
        }
        else ++it;
    }
    std::vector<DWORD> threads;
    bool needsHelper = false;
    for (const HWND window : windows)
    {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!thread || !process || process == GetCurrentProcessId() ||
            GetAncestor(window, GA_ROOT) != window) continue;
        const HANDLE previous = GetPropW(window, snowdesktop::dock_minimize::kTargetProperty);
        // Window properties outlive a crashed host. A recycled HWND without
        // the matching owner protocol is stale too; only a live host owns it.
        if (previous && previous != receiver_ &&
            snowdesktop::dock_minimize::HasLiveOwner(reinterpret_cast<HWND>(previous))) continue;
        if (!targets_.contains(window))
        {
            if (!SetPropW(window, snowdesktop::dock_minimize::kTargetProperty, receiver_)) continue;
            if (previous != receiver_ &&
                GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == previous)
                RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
            changed = true;
        }
        targets_[window] = process;
        if (snowdesktop::dock_minimize::Is32BitProcess(process)) needsHelper = true;
        else threads.push_back(thread);
    }
    hooks_.Sync(threads);
    if (changed)
        SetPropW(receiver_, snowdesktop::dock_minimize::kRevisionProperty, ULongToHandle(++targetRevision_));
    if (needsHelper) StartHelper();
    for (const HWND window : windows)
        if (OwnsTarget(window)) snowdesktop::dock_source_cloak::RecoverStale(window);
}

void DockExternalMinimize::StartHelper()
{
    if (helperProcess_)
    {
        if (WaitForSingleObject(helperProcess_, 0) == WAIT_TIMEOUT) return;
        CloseHandle(helperProcess_);
        helperProcess_ = nullptr;
        helperThread_ = 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (lastHelperAttempt_ && now - lastHelperAttempt_ < 10000) return;
    lastHelperAttempt_ = now;
    if (helperExecutable_.empty() || helperDll_.empty()) return;
    std::wstring command = L"\"" + helperExecutable_ + L"\" " +
        std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(HandleToULong(receiver_)) + L" \"" + helperDll_ + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helperExecutable_.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return;
    CloseHandle(process.hThread);
    helperProcess_ = process.hProcess;
    helperThread_ = process.dwThreadId;
}

void DockExternalMinimize::Stop() noexcept
{
    for (const auto& [window, process] : targets_)
    {
        (void)process;
        if (OwnsTarget(window))
        {
            const auto owner = snowdesktop::dock_source_cloak::ReadOwner(window);
            if (owner.process == ULongToHandle(GetCurrentProcessId()))
                snowdesktop::dock_source_cloak::Release(window, owner.window);
            if (GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == receiver_)
                RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
            RemovePropW(window, snowdesktop::dock_minimize::kTargetProperty);
        }
    }
    targets_.clear();
    if (receiver_ && GetPropW(receiver_, snowdesktop::dock_minimize::kOwnerProperty) ==
            ULongToHandle(GetCurrentProcessId()))
    {
        RemovePropW(receiver_, snowdesktop::dock_minimize::kOwnerProperty);
        RemovePropW(receiver_, snowdesktop::dock_minimize::kRevisionProperty);
    }
    receiver_ = nullptr;
    hooks_.Stop();
    if (helperProcess_)
    {
        // A child still initializing also observes the removed owner marker.
        PostThreadMessageW(helperThread_, WM_QUIT, 0, 0);
        WaitForSingleObject(helperProcess_, 500);
        CloseHandle(helperProcess_);
        helperProcess_ = nullptr;
    }
    helperThread_ = 0;
    lastHelperAttempt_ = 0;
}
