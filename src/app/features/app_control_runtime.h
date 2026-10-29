#pragma once

#include "ui/input/private_pointer.h"
#include "ui/render/demo_cursor.h"
#include "ui/preview/screen_capture.h"
#include "settings/settings_automation.h"
#include "demo/export_service.h"
#include <chrono>

namespace snowdesktop::app_control
{
// Application-only composition state. Keep adapter headers out of app.h so
// adding capture/automation services does not rebuild unrelated app modules.
struct Runtime
{
    private_pointer::Device privatePointer;
    bool privatePointerShowCursor = true;
    std::wstring privatePointerMonitor;
    std::string privatePointerSessionId;
    std::string privatePointerVisualError;
    std::chrono::steady_clock::time_point privatePointerStarted{};
    HWND privatePointerTimerHost = nullptr;
    UINT_PTR privatePointerTimer = 0;
    demo_cursor::Overlay cursor;
    screen_capture::Service capture;
    settings_automation::Service settings;
    demo::ExportService demo;
    ~Runtime() { Stop(); }
    void Stop() noexcept
    {
        if (privatePointerTimerHost && privatePointerTimer) KillTimer(privatePointerTimerHost, privatePointerTimer);
        privatePointerTimer = 0; privatePointerTimerHost = nullptr;
        privatePointer.SetObserver({}); privatePointer.End(); cursor.Hide(); capture.Stop(); settings.Stop(); demo.Stop();
    }
};
}
