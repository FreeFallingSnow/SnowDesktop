#pragma once

#include "../../steam_bridge/src/widget_multiplayer_wire.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace snowdesktop::steam_widget_multiplayer
{
// Owns the matching Bridge child and its private pipes. Only the worker waits
// for Steam or pipe traffic. The caller owns component/session routing.
class Service final
{
public:
    Service(std::filesystem::path executable, std::function<void()> wake);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    bool Submit(Frame frame);
    std::vector<Frame> Drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
