#include "diagnostics/performance_capture.h"
#include "diagnostics/performance_trace.h"
#include "widget/runtime/lua_runtime.h"
#include "widget/tasks/widget_task_broker.h"
#include "widget/tasks/widget_filesystem_task_executor.h"
#include "ui/render/ui_animation_scheduler.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace
{
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}
// Executes real allocator, OS stat, executor worker and scheduler boundaries.
// The only artificial input is an intentional 40ms UI-thread blockage.
void ExercisePerformanceWorkloads(const std::filesystem::path& root)
{
    namespace perf = snowdesktop::performance;
    namespace runtime = snowdesktop::widget_runtime;
    {
        perf::Scope parent("widget.event", "known.parent", L"nested-instance");
        { perf::Scope child("lua", "known.child"); std::this_thread::yield(); }
    }
    LuaRuntimeQuota quota; quota.memoryLimit = 1024;
    snowdesktop::lua_runtime::SampleAllocationTraffic(quota, L"allocator-instance");
    void* pointer = snowdesktop::lua_runtime::QuotaAllocator(&quota, nullptr, 999, 20);
    Require(pointer != nullptr, "real quota allocator accepts initial request");
    pointer = snowdesktop::lua_runtime::QuotaAllocator(&quota, pointer, 20, 40);
    Require(pointer != nullptr, "real quota allocator accepts realloc");
    Require(!snowdesktop::lua_runtime::QuotaAllocator(&quota, pointer, 40, 2048), "quota rejects excessive realloc without changing live allocation");
    snowdesktop::lua_runtime::QuotaAllocator(&quota, pointer, 40, 0);
    const auto& traffic = quota.allocationTraffic;
    Require(traffic.allocatedBytes == 60 && traffic.freedBytes == 60 && traffic.allocations == 1 &&
        traffic.reallocations == 1 && traffic.frees == 1 && traffic.peakBytes == 40 && !quota.memoryBytes,
        "allocator counts successful logical traffic, excludes failed request and preserves live bytes");
    snowdesktop::lua_runtime::SampleAllocationTraffic(quota, L"allocator-instance");
    const auto path = root / L"stat-fixture.txt";
    { std::ofstream file(path, std::ios::binary); file << "fixture"; }
    runtime::WidgetTaskBroker broker; std::string error;
    Require(broker.RegisterTask({"filesystem.stat", {}, false, 4}, error), "register fixture task");
    runtime::TaskStartOptions options; options.ownerToken = 7;
    const auto task = broker.Start("worker-instance", "filesystem.stat", std::move(options));
    Require(static_cast<bool>(task), "real task broker accepts owner");
    runtime::WidgetFilesystemTaskExecutor executor;
    runtime::WidgetFilesystemTaskRequest request; request.action = "filesystem.stat"; request.path = path;
    Require(static_cast<bool>(executor.Start(task.id, "worker-instance", request)), "actual executor queues OS stat");
    const auto taskDeadline = GetTickCount64() + 5000;
    bool completed = false;
    while (!completed && GetTickCount64() < taskDeadline)
    {
        for (const auto& result : executor.DrainCompletions())
        {
            Require(result.id == task.id && result.ok && result.metadata.size == 7, "OS stat worker preserves exact result");
            Require(broker.Complete(result.id, result.ok), "broker delivers completion"); completed = true;
        }
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(completed, "actual worker completes within deadline");
    snowdesktop::UiAnimationScheduler scheduler;
    Require(scheduler.Initialize(), "initialize isolated scheduler timer");
    int sharedOwner = 0, presentations = 0, first = 0, second = 0;
    const auto present = [&] { ++presentations; scheduler.RecordCommitDuration(0.1); };
    scheduler.StartAnimation(snowdesktop::UiAnimationSurface::Desktop, [&](double) {
        scheduler.RequestFramePresentation(&sharedOwner, present); return ++first < 4;
    });
    scheduler.StartAnimation(snowdesktop::UiAnimationSurface::Popup, [&](double) {
        scheduler.RequestFramePresentation(&sharedOwner, present); return ++second < 4;
    });
    scheduler.ScheduleOnce(0, [](auto) { std::this_thread::sleep_for(std::chrono::milliseconds(40)); });
    const auto animationDeadline = GetTickCount64() + 5000;
    while (scheduler.HasScheduledWork() && GetTickCount64() < animationDeadline)
    {
        if (WaitForSingleObject(scheduler.WaitHandle(), 100) == WAIT_OBJECT_0) scheduler.DispatchDue();
    }
    Require(!scheduler.HasScheduledWork() && first == 4 && second == 4 && presentations == 4,
        "two real animations share one presentation per delivered frame");
}
