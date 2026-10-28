#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::controlled_pointer
{
struct Sample
{
    double milliseconds = 0;
    POINT point{};
    unsigned buttons = 0;
    std::string phase;
};
enum class Action { Hover, Click, DoubleClick, ContextMenu, Drag, Scroll };
struct Plan
{
    Action action = Action::Hover;
    std::string sourceId, destinationId;
    double durationMilliseconds = 400;
    int wheelDelta = 0;
    bool showCursor = false;
};
struct Target { RECT bounds{}; HWND window = nullptr; };
POINT Interpolate(POINT from, POINT to, double progress, double curvature = 0.12);
bool AbsolutePoint(POINT point, RECT virtualScreen, LONG& x, LONG& y);

struct Interruption
{
    std::string device;
    UINT message = 0;
    DWORD flags = 0, virtualKey = 0;
    std::uint64_t extraInfo = 0, expectedExtraInfo = 0;
    POINT point{};
    bool injected = false, lowerIntegrity = false;
};
struct State
{
    std::uint64_t operation = 0;
    std::string status = "idle", error;
    Plan plan;
    Sample current;
    std::vector<Sample> trace;
    std::optional<Interruption> interruption;
};

class Player final
{
public:
    using Resolve = std::function<Target(const std::string&)>;
    using Observe = std::function<void(const State&)>;
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    bool Start(Plan plan, Resolve resolve, Observe observe, std::string& error);
    void Cancel(std::string reason = "cancelled");
    void Stop() noexcept;
    const State& Current() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
