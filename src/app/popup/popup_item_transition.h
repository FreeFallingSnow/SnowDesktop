#pragma once

#include "dock/dock_running_animation.h"

#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace snowdesktop::popup_item_transition
{
struct Visual
{
    RECT bounds{};
    float opacity = 1.0f;
};

struct Entry
{
    std::wstring path;
    Visual visual;
};

struct Frame
{
    RECT panel{};
    RECT content{};
    int scroll = 0;
    std::vector<Entry> entries;
};

class Transition final
{
public:
    Transition(Frame from, Frame target, double now, double durationScale);
    bool Advance(double now);
    RECT Panel(RECT target) const;
    RECT ReservedPanel(RECT target) const;
    RECT Content(RECT target, RECT layoutPanel) const;
    Visual Item(std::size_t index, const std::wstring& path, RECT target,
        RECT layoutPanel, int scroll) const;
    float ItemOpacity(std::size_t index, const std::wstring& path, RECT layoutPanel, int scroll) const;

private:
    const Visual* FindStart(std::size_t index, const std::wstring& path, RECT layoutPanel, int scroll) const;
    Frame target_;
    RECT fromPanel_{};
    RECT fromContent_{};
    std::vector<Visual> fromItems_;
    dock_running_animation::Presence presence_;
};
}
