#pragma once

#include <memory>
#include <string_view>

struct ID2D1Device;
struct ID3D11Device;

namespace snowdesktop::widget_runtime
{
// Render-thread-only, capture-only timestamp ranges. Readback never waits or
// flushes the GPU. Measures this host's raster commands, not DWM composition.
class WidgetGpuTiming final
{
public:
    WidgetGpuTiming();
    ~WidgetGpuTiming();
    WidgetGpuTiming(const WidgetGpuTiming&) = delete;
    WidgetGpuTiming& operator=(const WidgetGpuTiming&) = delete;
    void Initialize(ID3D11Device* graphics, ID2D1Device* drawing);
    void Reset();

    class Frame final
    {
    public:
        Frame(WidgetGpuTiming& timer, std::wstring_view owner);
        ~Frame();
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        void Complete(bool successful);
        bool Active() const noexcept { return timer_ != nullptr; }
    private:
        WidgetGpuTiming* timer_ = nullptr;
    };

private:
    bool Begin(std::wstring_view owner);
    void End(bool successful) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
