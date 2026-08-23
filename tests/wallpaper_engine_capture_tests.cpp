#include "app/wallpaper_engine_capture.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace
{

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using snowdesktop::wallpaper_engine_capture::CropFrameToDesktopRegion;
    using snowdesktop::wallpaper_engine_capture::CancellableWaitResult;
    using snowdesktop::wallpaper_engine_capture::WaitForHandleOrCancellation;

    HANDLE waitEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    Check(waitEvent != nullptr,
        "cancellable wait test event is available");
    Check(WaitForHandleOrCancellation(waitEvent, 100) ==
            CancellableWaitResult::Signaled,
        "cancellable wait reports a signaled handle");
    ResetEvent(waitEvent);
    Check(WaitForHandleOrCancellation(waitEvent, 20) ==
            CancellableWaitResult::TimedOut,
        "cancellable wait preserves finite timeouts");
    std::atomic_bool cancelWait = false;
    std::thread cancelThread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancelWait.store(true, std::memory_order_relaxed);
    });
    const auto waitStarted = std::chrono::steady_clock::now();
    const auto cancelledResult = WaitForHandleOrCancellation(
        waitEvent, 2000, &cancelWait);
    const auto cancelledElapsed = std::chrono::steady_clock::now() -
        waitStarted;
    cancelThread.join();
    CloseHandle(waitEvent);
    Check(cancelledResult == CancellableWaitResult::Cancelled &&
            cancelledElapsed < std::chrono::milliseconds(750),
        "cancellable wait exits promptly without consuming its full timeout");

    const std::vector<std::uint32_t> source{
        0x00112233u, 0x00445566u, 0x00778899u, 0x00aabbccu,
        0x00010203u, 0x00040506u, 0x00070809u, 0x000a0b0cu,
    };
    const RECT sourceBounds{ -2, 3, 2, 5 };
    const RECT centerBounds{ -1, 3, 1, 5 };
    const auto center = CropFrameToDesktopRegion(source.data(), 4, 2,
        sourceBounds, centerBounds);
    Check(center.width == 2 && center.height == 2 &&
            EqualRect(&center.desktopBounds, &centerBounds),
        "one-shot frame crop preserves physical desktop bounds");
    Check(center.pixels == std::vector<std::uint32_t>{
            0xff445566u, 0xff778899u,
            0xff040506u, 0xff070809u },
        "one-shot frame crop selects the matching source pixels and forces opacity");

    const std::vector<std::uint32_t> scaledSource{
        0x00102030u, 0x00405060u,
    };
    const RECT scaledDesktop{ 0, 0, 4, 2 };
    const auto scaled = CropFrameToDesktopRegion(scaledSource.data(), 2, 1,
        scaledDesktop, scaledDesktop);
    Check(scaled.width == 4 && scaled.height == 2 &&
            scaled.pixels == std::vector<std::uint32_t>{
                0xff102030u, 0xff102030u,
                0xff405060u, 0xff405060u,
                0xff102030u, 0xff102030u,
                0xff405060u, 0xff405060u },
        "one-shot frame crop maps renderer pixels back to physical monitor coordinates");

    const RECT partialRequest{ 1, 4, 6, 8 };
    const RECT expectedIntersection{ 1, 4, 2, 5 };
    const auto partial = CropFrameToDesktopRegion(source.data(), 4, 2,
        sourceBounds, partialRequest);
    Check(partial.width == 1 && partial.height == 1 &&
            EqualRect(&partial.desktopBounds, &expectedIntersection) &&
            partial.pixels.front() == 0xff0a0b0cu,
        "one-shot frame crop clips requests to the published output rectangle");

    const RECT outside{ 20, 20, 30, 30 };
    Check(CropFrameToDesktopRegion(source.data(), 4, 2,
            sourceBounds, outside).Empty() &&
          CropFrameToDesktopRegion(nullptr, 4, 2,
            sourceBounds, centerBounds).Empty(),
        "one-shot frame crop rejects missing and non-overlapping sources");

    std::cout << "wallpaper engine capture tests passed\n";
    return 0;
}
