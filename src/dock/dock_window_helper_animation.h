#pragma once

#include "platform/window_support/window_support_protocol.h"
#include "ui/render/ui_animation_scheduler.h"
#include <memory>

// The elevated helper owns both the source-thread hooks and the presentation
// lease. Losing the host connection destroys this object and releases both.
class DockWindowHelperAnimation final
{
public:
    DockWindowHelperAnimation();
    ~DockWindowHelperAnimation();
    bool Initialize();
    bool Available() const noexcept;
    snowdesktop::UiAnimationScheduler& Scheduler();
    bool Configure(const snowdesktop::window_support::protocol::Request& request);
    bool Start(const snowdesktop::window_support::protocol::Target& target, bool restore);
    void Cancel(bool complete);
    void Snapshot(snowdesktop::window_support::protocol::Response& response);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
