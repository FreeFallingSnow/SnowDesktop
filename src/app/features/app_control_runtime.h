#pragma once

#include "ui/input/control_pointer.h"
#include "ui/render/demo_cursor.h"

namespace snowdesktop::app_control
{
// Application-only composition state. Keep adapter headers out of app.h so
// adding capture/automation services does not rebuild unrelated app modules.
struct Runtime
{
    controlled_pointer::Player pointer;
    demo_cursor::Overlay cursor;
    ~Runtime() { Stop(); }
    void Stop() noexcept { pointer.Stop(); cursor.Hide(); }
};
}
