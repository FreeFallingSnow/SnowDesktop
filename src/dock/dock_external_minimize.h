#pragma once

#include "dock_minimize_hook_threads.h"
#include "dock_minimize_process_cache.h"
#include <unordered_map>

// Owns only the HWND properties and thread hooks installed by this host.
// Paths must already refer to process-private deployment copies.
class DockExternalMinimize final
{
public:
    ~DockExternalMinimize() { Stop(); }
    bool Start(HWND receiver, const std::wstring& nativeDll,
        std::wstring helperExecutable, std::wstring helperDll);
    void UpdateTargets(std::span<const HWND> windows);
    void Stop() noexcept;
    bool OwnsTarget(HWND window) const noexcept;
    bool IsMinimizeButtonPress(HWND window, POINT screenPoint) const noexcept;
    std::size_t HookCount() const noexcept { return hooks_.HookCount(); }
    snowdesktop::hook_registration::Statistics RegistrationStatistics() const noexcept { return hooks_.RegistrationStatistics(); }
    snowdesktop::hook_registration::Statistics CloakStatistics() const { return hooks_.CloakStatistics(); }
    std::size_t CloakHookCount() const { return hooks_.CloakHookCount(); }

private:
    void StartHelper();
    DWORD OwnedTargetThread(HWND window) const noexcept;
    HWND receiver_ = nullptr;
    snowdesktop::dock_minimize::ThreadHooks hooks_;
    snowdesktop::dock_minimize::ProcessBitnessCache processBitness_;
    std::unordered_map<HWND, DWORD> targets_;
    std::wstring helperExecutable_;
    std::wstring helperDll_;
    HANDLE helperProcess_ = nullptr;
    DWORD helperThread_ = 0;
    ULONGLONG lastHelperAttempt_ = 0;
    DWORD targetRevision_ = 0;
};
