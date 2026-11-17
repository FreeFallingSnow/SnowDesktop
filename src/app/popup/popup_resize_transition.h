#pragma once

#include "dock/dock_running_animation.h"

#include <windows.h>

namespace snowdesktop::popup_resize_transition
{
struct Geometry
{
    RECT panel{};
    RECT content{};
};

class Transition final
{
public:
    Transition(Geometry from, Geometry target, double now, double durationScale);
    bool Advance(double now);
    bool Targets(const Geometry& geometry) const;
    RECT Panel(RECT target) const;
    RECT ReservedPanel(RECT target) const;
    RECT Content(RECT target, RECT layoutPanel) const;

private:
    Geometry from_;
    Geometry target_;
    dock_running_animation::Presence progress_;
};
}
