#pragma once

#include "start_menu_position.h"
#include <atomic>
#include <memory>

namespace snowdesktop
{
// Owns one Dock-origin invocation. Native keyboard/taskbar invocations never
// acquire this session. The worker owns all cross-process waits and cleanup.
class NativeStartMenu final
{
public:
    NativeStartMenu();
    ~NativeStartMenu();
    NativeStartMenu(const NativeStartMenu&) = delete;
    NativeStartMenu& operator=(const NativeStartMenu&) = delete;
    bool Open(HWND owner, HMONITOR monitor, const start_menu::Placement& placement);
    void Cancel(bool dismiss = false) noexcept;
    bool Active() const noexcept;
    HMONITOR Monitor() const noexcept;
    bool ContainsPoint(POINT screen) const noexcept;
    static bool Toggle() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
