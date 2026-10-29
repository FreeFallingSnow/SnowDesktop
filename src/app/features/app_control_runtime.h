#pragma once

#include "ui/input/private_pointer.h"
#include "ui/render/demo_cursor.h"
#include "ui/preview/screen_capture.h"
#include "settings/settings_automation.h"
#include "demo/export_service.h"

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
    demo_cursor::Overlay cursor;
    screen_capture::Service capture;
    settings_automation::Service settings;
    demo::ExportService demo;
    ~Runtime() { Stop(); }
    void Stop() noexcept { privatePointer.SetObserver({}); privatePointer.End(); cursor.Hide(); capture.Stop(); settings.Stop(); demo.Stop(); }
};
}
