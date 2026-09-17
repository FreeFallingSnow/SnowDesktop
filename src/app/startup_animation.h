#pragma once

#include <windows.h>
#include "startup_cancellation.h"
#include <string>
#include <thread>

namespace snowdesktop
{
// Host-private, optional startup presentation. No desktop model or Explorer
// input queue is shared with its UI thread; finishing never waits on that UI.
class StartupAnimation final
{
public:
    StartupAnimation() = default;
    ~StartupAnimation();
    StartupAnimation(const StartupAnimation&) = delete;
    StartupAnimation& operator=(const StartupAnimation&) = delete;

    bool Start(HINSTANCE instance, HWND desktopHost,
        bool animate, double durationScale,
        std::wstring startingText, std::wstring cancelText);
    bool BeginDesktopHandoff() noexcept;
    void Finish() noexcept;

private:
    HANDLE stopEvent_ = nullptr;
    HANDLE finishEvent_ = nullptr;
    HANDLE handoffEvent_ = nullptr;
    StartupCancellation cancellation_;
    std::thread thread_;
};
}
