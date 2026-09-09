#pragma once

#include <windows.h>
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
        bool animate, double durationScale);
    void Finish() noexcept;

private:
    HANDLE stopEvent_ = nullptr;
    HANDLE finishEvent_ = nullptr;
    std::thread thread_;
};
}
