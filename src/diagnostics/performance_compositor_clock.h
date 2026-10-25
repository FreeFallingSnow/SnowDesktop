#pragma once
#include <memory>
namespace snowdesktop::performance
{
// Optional Windows 11 display-compositor telemetry; never changes its cadence.
class CompositorClock final
{
public:
    CompositorClock();
    ~CompositorClock();
    void Sample();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
