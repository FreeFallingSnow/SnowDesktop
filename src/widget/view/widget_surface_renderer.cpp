#include "widget_surface_renderer.h"
#include "widget_gpu_timing.h"
#include "diagnostics/performance_trace.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

struct WidgetSurfaceTarget
{
    std::wstring owner;
    std::wstring componentOwner;
    UINT width = 0, height = 0;
    ComPtr<IDCompositionVisual2> visual;
    // Only the render thread accesses these surface/notification fields.
    ComPtr<IDCompositionSurface> surface;
    bool announced = false;
    std::atomic<bool> active{true}, failed{false};
};

struct WidgetSurfaceRenderer::Impl
{
    struct Frame
    {
        Target target;
        ComPtr<ID2D1CommandList> commands;
        RECT bounds{};
    };
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> recording;
    ComPtr<IDCompositionDesktopDevice> composition;
    WidgetGpuTiming gpuTiming;
    HWND notifyWindow = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Frame> pending;
    std::vector<Result> ready;
    std::thread worker;
    bool stopping = false;

    HRESULT Draw(const Frame& frame)
    {
        auto& target = *frame.target;
        performance::Scope scope("widget.composition", "raster", target.componentOwner);
        if (!target.surface)
        {
            const HRESULT result = composition->CreateSurface(target.width,
                target.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED, &target.surface);
            if (FAILED(result)) return result;
        }
        ComPtr<ID2D1DeviceContext> context;
        WidgetGpuTiming::Frame gpuFrame(gpuTiming, target.componentOwner);
        POINT offset{};
        HRESULT result = target.surface->BeginDraw(nullptr,
            __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(context.GetAddressOf()), &offset);
        if (FAILED(result)) return result;
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x - frame.bounds.left),
            static_cast<float>(offset.y - frame.bounds.top)));
        context->Clear(D2D1::ColorF(0, 0.0f));
        context->DrawImage(frame.commands.Get());
        context.Reset();
        result = target.surface->EndDraw();
        if (FAILED(result) || !target.active.load()) return result;
        result = target.visual->SetContent(target.surface.Get());
        if (SUCCEEDED(result)) result = composition->Commit();
        // DComp submits the D2D surface work at Commit. An earlier timestamp
        // only measures an empty command range, despite valid query readback.
        gpuFrame.Complete(SUCCEEDED(result));
        return result;
    }

    void Run() noexcept
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;)
        {
            Frame frame;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || !pending.empty(); });
                if (stopping) break;
                frame = std::move(pending.front());
                pending.pop_front();
            }
            if (!frame.target->active.load()) continue;
            HRESULT result = apartment;
            try
            {
                if (SUCCEEDED(result)) result = Draw(frame);
            }
            catch (...) { result = E_FAIL; }
            if (!frame.target->active.load()) continue;
            if (FAILED(result)) frame.target->failed.store(true);
            if (FAILED(result) || !frame.target->announced)
            {
                frame.target->announced = true;
                bool notify = false;
                try
                {
                    std::lock_guard lock(mutex);
                    if (!stopping && frame.target->active.load())
                    {
                        notify = ready.empty();
                        ready.push_back({frame.target, frame.target->owner, result});
                    }
                }
                catch (...) { frame.target->failed.store(true); }
                if (notify) PostMessageW(notifyWindow, ReadyMessage(), 0, 0);
            }
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};

WidgetSurfaceRenderer::WidgetSurfaceRenderer() : impl_(std::make_unique<Impl>()) {}
WidgetSurfaceRenderer::~WidgetSurfaceRenderer() { Stop(); }

UINT WidgetSurfaceRenderer::ReadyMessage()
{
    static const UINT message = RegisterWindowMessageW(
        L"FreeFallingSnow.SnowDesktop.WidgetSurfaceReady.v1");
    return message;
}

bool WidgetSurfaceRenderer::Initialize(ID2D1Device* device, HWND notifyWindow, ID3D11Device* graphicsDevice)
{
    if (!device || !notifyWindow || !ReadyMessage() || impl_->worker.joinable()) return false;
    ComPtr<ID2D1Factory> factory;
    device->GetFactory(&factory);
    ComPtr<ID2D1Multithread> multithread;
    if (!factory || FAILED(factory.As(&multithread)) ||
        !multithread->GetMultithreadProtected()) return false;
    impl_->device = device;
    impl_->notifyWindow = notifyWindow;
    impl_->stopping = false;
    impl_->gpuTiming.Initialize(graphicsDevice, device);
    HRESULT result = device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->recording);
    if (SUCCEEDED(result)) result = DCompositionCreateDevice3(device,
        __uuidof(IDCompositionDesktopDevice),
        reinterpret_cast<void**>(impl_->composition.GetAddressOf()));
    if (FAILED(result)) return false;
    try { impl_->worker = std::thread([this] { impl_->Run(); }); }
    catch (...) { return false; }
    return true;
}

void WidgetSurfaceRenderer::Stop()
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        for (const auto& frame : impl_->pending) frame.target->active.store(false);
        impl_->pending.clear();
        impl_->ready.clear();
    }
    impl_->changed.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->gpuTiming.Reset();
    impl_->recording.Reset();
    impl_->composition.Reset();
    impl_->device.Reset();
}

HRESULT WidgetSurfaceRenderer::BeginRecording(ID2D1DeviceContext** context,
    ID2D1CommandList** commands)
{
    performance::Scope scope("widget.composition", "commands.begin");
    if (!context || !commands || !impl_->recording) return E_INVALIDARG;
    *context = nullptr; *commands = nullptr;
    HRESULT result = impl_->recording->CreateCommandList(commands);
    if (FAILED(result)) return result;
    impl_->recording->SetDpi(96.0f, 96.0f);
    impl_->recording->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    impl_->recording->SetTransform(D2D1::Matrix3x2F::Identity());
    impl_->recording->SetTarget(*commands);
    impl_->recording->BeginDraw();
    return impl_->recording.CopyTo(context);
}

HRESULT WidgetSurfaceRenderer::EndRecording(ID2D1CommandList* commands)
{
    performance::Scope scope("widget.composition", "commands.close");
    if (!commands || !impl_->recording) return E_INVALIDARG;
    const HRESULT result = impl_->recording->EndDraw();
    impl_->recording->SetTarget(nullptr);
    const HRESULT closed = commands->Close();
    return FAILED(result) ? result : closed;
}

WidgetSurfaceRenderer::Target WidgetSurfaceRenderer::CreateTarget(
    const std::wstring& owner, UINT width, UINT height, IDCompositionVisual2** visual,
    const std::wstring& componentOwner)
{
    if (!visual || !width || !height || !impl_->composition) return {};
    *visual = nullptr;
    auto target = std::make_shared<WidgetSurfaceTarget>();
    target->owner = owner; target->width = width; target->height = height;
    target->componentOwner = componentOwner.empty() ? owner : componentOwner;
    if (FAILED(impl_->composition->CreateVisual(&target->visual)) ||
        FAILED(target->visual.CopyTo(visual))) return {};
    return target;
}

bool WidgetSurfaceRenderer::Submit(const Target& target,
    ID2D1CommandList* commands, const RECT& bounds)
{
    performance::Scope scope("widget.composition", "commands.submit");
    if (!target || !commands || !target->active.load() || target->failed.load()) return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return false;
        auto found = std::find_if(impl_->pending.begin(), impl_->pending.end(),
            [&target](const auto& frame) { return frame.target == target; });
        if (found != impl_->pending.end())
            *found = {target, commands, bounds};
        else
        {
            // One queued frame per visible owner, plus the in-flight frame.
            // A burst cannot retain an unbounded history of images/commands.
            if (impl_->pending.size() >= 128) return false;
            impl_->pending.push_back({target, commands, bounds});
        }
    }
    impl_->changed.notify_one();
    return true;
}

void WidgetSurfaceRenderer::Cancel(const Target& target)
{
    if (!target) return;
    target->active.store(false);
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->pending, [&target](const auto& frame) { return frame.target == target; });
    std::erase_if(impl_->ready, [&target](const auto& result) { return result.target == target; });
}

std::vector<WidgetSurfaceRenderer::Result> WidgetSurfaceRenderer::Drain()
{
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->ready, {});
}
}
