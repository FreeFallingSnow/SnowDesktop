#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>

namespace snowdesktop
{
// Owns one Dock-origin invocation. Native keyboard/taskbar invocations never
// acquire this session. The worker owns all cross-process waits and cleanup.
class NativeStartMenu final
{
public:
    enum class CancelReason { Requested, Dismissed, ConfigurationChanged };
    NativeStartMenu();
    ~NativeStartMenu();
    NativeStartMenu(const NativeStartMenu&) = delete;
    NativeStartMenu& operator=(const NativeStartMenu&) = delete;
    bool Open(HMONITOR monitor);
    void Cancel(bool dismiss = false, CancelReason reason = CancelReason::Requested) noexcept;
    // UI only. Replaces a pending action; runs it after confirmed dismissal
    // and UI cleanup. False means no session needs to be dismissed.
    bool DismissThen(std::function<void()> action);
    // UI thread only: release the Dock before dispatching deferred fallback.
    bool Finish(const std::function<void()>& releaseHold, const std::function<void()>& nativeFallback);
    bool Active() const noexcept;
    HMONITOR Monitor() const noexcept;
    bool ContainsPoint(POINT screen) const noexcept;
    // UI sampler: disregard the opening press, dismiss on later owned surfaces.
    void ObservePointer(bool down, bool pressed, bool ownedSurface) noexcept;
    static bool Toggle() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
