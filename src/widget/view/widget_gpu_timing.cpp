#include "widget_gpu_timing.h"
#include "diagnostics/performance_trace.h"
#include "diagnostics/performance_trace_control.h"

#include <d3d11.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;
struct WidgetGpuTiming::Impl
{
    struct Query
    {
        ComPtr<ID3D11Query> disjoint, start, end;
        std::wstring owner;
        bool successful = false;
    };
    struct Totals
    {
        double milliseconds = 0;
        std::uint64_t attempts = 0, samples = 0, disjoint = 0, errors = 0, skipped = 0;
    };
    ComPtr<ID3D11Device> graphics;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID2D1Multithread> lock;
    std::deque<Query> pending;
    std::vector<Query> available;
    std::unordered_map<std::wstring, Totals> totals;
    Query current;
    std::uint64_t generation = 0;
    bool entered = false;

    static void Emit(const std::wstring& owner, const Totals& value)
    {
        performance::Value("widget.gpu", "raster_ms_total", owner, value.milliseconds);
        performance::Value("widget.gpu", "ranges_total", owner, static_cast<double>(value.attempts));
        performance::Value("widget.gpu", "samples_total", owner, static_cast<double>(value.samples));
        performance::Value("widget.gpu", "disjoint_total", owner, static_cast<double>(value.disjoint));
        performance::Value("widget.gpu", "errors_total", owner, static_cast<double>(value.errors));
        performance::Value("widget.gpu", "skipped_total", owner, static_cast<double>(value.skipped));
    }
    void Clear()
    {
        pending.clear(); available.clear(); totals.clear(); current = {};
    }
    void Poll()
    {
        // The shared D2D factory lock is acquired before any direct D3D call,
        // matching D2D's own order. No UI work can enter the measured command
        // range. This short lock never surrounds a wait/GetData retry loop.
        while (!pending.empty())
        {
            auto& query = pending.front();
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
            UINT64 start = 0, end = 0;
            lock->Enter();
            const HRESULT clockResult = context->GetData(query.disjoint.Get(), &clock,
                sizeof(clock), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT startResult = context->GetData(query.start.Get(), &start,
                sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT endResult = context->GetData(query.end.Get(), &end,
                sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            lock->Leave();
            const bool failed = FAILED(clockResult) || FAILED(startResult) || FAILED(endResult);
            if (!failed && (clockResult == S_FALSE || startResult == S_FALSE || endResult == S_FALSE)) break;
            auto& value = totals.at(query.owner);
            if (FAILED(clockResult) || FAILED(startResult) || FAILED(endResult) || !query.successful)
                ++value.errors;
            else if (clock.Disjoint || !clock.Frequency || end < start)
                ++value.disjoint;
            else
            {
                value.milliseconds += static_cast<double>(end - start) * 1000.0 /
                    static_cast<double>(clock.Frequency);
                ++value.samples;
            }
            Emit(query.owner, value);
            auto completed = std::move(query);
            pending.pop_front();
            // Failure to retain a reusable query must not replay its result.
            try { available.push_back(std::move(completed)); } catch (...) {}
        }
    }
};

WidgetGpuTiming::WidgetGpuTiming() : impl_(std::make_unique<Impl>()) {}
WidgetGpuTiming::~WidgetGpuTiming() = default;

void WidgetGpuTiming::Initialize(ID3D11Device* graphics, ID2D1Device* drawing)
{
    Reset();
    if (!graphics || !drawing) return;
    ComPtr<ID2D1Factory> factory; drawing->GetFactory(&factory);
    if (!factory || FAILED(factory.As(&impl_->lock)) ||
        !impl_->lock->GetMultithreadProtected()) return;
    impl_->graphics = graphics;
    graphics->GetImmediateContext(&impl_->context);
}
void WidgetGpuTiming::Reset()
{
    if (impl_->entered) End(false);
    impl_->Clear(); impl_->generation = 0;
    impl_->context.Reset(); impl_->graphics.Reset(); impl_->lock.Reset();
}

bool WidgetGpuTiming::Begin(std::wstring_view owner)
{
    const auto generation = performance::CaptureGeneration();
    if (generation != impl_->generation)
    {
        impl_->Clear();
        impl_->generation = generation;
    }
    if (!generation || !impl_->context || !impl_->lock || owner.empty()) return false;
    impl_->Poll();
    if (impl_->totals.size() >= 1024 && !impl_->totals.contains(std::wstring(owner))) return false;
    auto [position, inserted] = impl_->totals.try_emplace(std::wstring(owner));
    if (inserted) Impl::Emit(position->first, position->second);
    auto& value = position->second;
    ++value.attempts;
    if (impl_->pending.size() >= 64)
    {
        ++value.skipped; Impl::Emit(position->first, value); return false;
    }
    Impl::Query query;
    if (!impl_->available.empty())
    {
        query = std::move(impl_->available.back());
        impl_->available.pop_back();
    }
    else
    {
        D3D11_QUERY_DESC descriptor{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HRESULT result = impl_->graphics->CreateQuery(&descriptor, &query.disjoint);
        descriptor.Query = D3D11_QUERY_TIMESTAMP;
        if (SUCCEEDED(result)) result = impl_->graphics->CreateQuery(&descriptor, &query.start);
        if (SUCCEEDED(result)) result = impl_->graphics->CreateQuery(&descriptor, &query.end);
        if (FAILED(result))
        {
            ++value.errors; Impl::Emit(position->first, value); return false;
        }
    }
    query.owner = owner;
    query.successful = false;
    impl_->current = std::move(query);
    // Counter publication is outside the factory's critical section.
    Impl::Emit(position->first, value);
    impl_->lock->Enter(); impl_->entered = true;
    impl_->context->Begin(impl_->current.disjoint.Get());
    impl_->context->End(impl_->current.start.Get());
    return true;
}
void WidgetGpuTiming::End(bool successful) noexcept
{
    impl_->context->End(impl_->current.end.Get());
    impl_->context->End(impl_->current.disjoint.Get());
    impl_->lock->Leave(); impl_->entered = false;
    impl_->current.successful = successful;
    auto query = std::exchange(impl_->current, {});
    try { impl_->pending.push_back(std::move(query)); }
    catch (...)
    {
        if (const auto found = impl_->totals.find(query.owner); found != impl_->totals.end())
        { ++found->second.skipped; Impl::Emit(found->first, found->second); }
    }
}
WidgetGpuTiming::Frame::Frame(WidgetGpuTiming& timer, std::wstring_view owner)
{
    try { if (timer.Begin(owner)) timer_ = &timer; }
    catch (...) { if (timer.impl_->entered) timer.End(false); }
}
WidgetGpuTiming::Frame::~Frame() { if (timer_) timer_->End(false); }
void WidgetGpuTiming::Frame::Complete(bool successful)
{
    if (timer_) { timer_->End(successful); timer_ = nullptr; }
}
}
