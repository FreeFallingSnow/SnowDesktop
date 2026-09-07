#pragma once

#include <windows.h>
#include <dwmapi.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace snowdesktop::dock_snapshot_warmup
{
// Host-private, top-down opaque BGRA pixels. This is an opportunistic snapshot
// of a fully visible foreground window, never a way to expose hidden content.
struct Frame
{
    HWND window = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;
    RECT sourceRect{};
    SIZE pixelSize{};
    WINDOWPLACEMENT placement{};
    ULONGLONG capturedTick = 0;
    std::vector<std::uint32_t> pixels;
};

struct Request
{
    std::atomic<bool> ready{false};
    std::atomic<bool> cancelled{false};
    // Read only after ready.load(memory_order_acquire). An empty pixel vector
    // means capture was rejected, failed or cancelled. No worker owns the host.
    Frame frame;
};

namespace detail
{
class ScopedPhysicalCoordinates
{
public:
    ScopedPhysicalCoordinates() noexcept
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
    }
    ~ScopedPhysicalCoordinates() noexcept
    {
        if (previous_)
            SetThreadDpiAwarenessContext(previous_);
    }
    ScopedPhysicalCoordinates(const ScopedPhysicalCoordinates&) = delete;
    ScopedPhysicalCoordinates& operator=(const ScopedPhysicalCoordinates&) = delete;
    explicit operator bool() const noexcept { return previous_ != nullptr; }

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

inline bool SameWindow(const Frame& frame) noexcept
{
    if (!frame.window || !frame.processId || !frame.threadId ||
        !IsWindow(frame.window) || GetAncestor(frame.window, GA_ROOT) != frame.window)
        return false;
    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(frame.window, &processId);
    return processId == frame.processId && threadId == frame.threadId;
}

inline bool ReadVisibleGeometry(HWND window, RECT& rect,
    WINDOWPLACEMENT& placement) noexcept
{
    if (!IsWindowVisible(window) || IsIconic(window))
        return false;
    DWORD affinity = WDA_NONE;
    if (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE)
        return false;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED,
            &cloaked, sizeof(cloaked))) || cloaked)
        return false;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &rect, sizeof(rect))) && !GetWindowRect(window, &rect))
        return false;
    placement = {};
    placement.length = sizeof(placement);
    return rect.right > rect.left && rect.bottom > rect.top &&
        GetWindowPlacement(window, &placement) != FALSE &&
        IsWindowVisible(window) && !IsIconic(window);
}

inline bool SameVisibleGeometry(const Frame& frame) noexcept
{
    RECT rect{};
    WINDOWPLACEMENT placement{};
    return frame.placement.length == sizeof(WINDOWPLACEMENT) &&
        ReadVisibleGeometry(frame.window, rect, placement) &&
        EqualRect(&rect, &frame.sourceRect) &&
        EqualRect(&placement.rcNormalPosition, &frame.placement.rcNormalPosition) &&
        (placement.showCmd == SW_SHOWMAXIMIZED) ==
            (frame.placement.showCmd == SW_SHOWMAXIMIZED);
}

struct MonitorCoverage
{
    HRGN uncovered = nullptr;
    bool valid = true;
};

inline BOOL CALLBACK SubtractMonitor(HMONITOR, HDC, LPRECT bounds,
    LPARAM parameter) noexcept
{
    auto& coverage = *reinterpret_cast<MonitorCoverage*>(parameter);
    if (!bounds)
    {
        coverage.valid = false;
        return FALSE;
    }
    HRGN monitor = CreateRectRgnIndirect(bounds);
    if (!monitor)
    {
        coverage.valid = false;
        return FALSE;
    }
    coverage.valid = CombineRgn(coverage.uncovered, coverage.uncovered,
        monitor, RGN_DIFF) != ERROR;
    DeleteObject(monitor);
    return coverage.valid ? TRUE : FALSE;
}

inline bool InsideVirtualScreen(const RECT& rect) noexcept
{
    const std::int64_t left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const std::int64_t top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0 || rect.left < left || rect.top < top ||
        rect.right > left + width || rect.bottom > top + height)
        return false;
    // The virtual-screen bounding box includes gaps between staggered monitors.
    // Require the complete source to be covered by the actual monitor union.
    MonitorCoverage coverage{CreateRectRgnIndirect(&rect)};
    if (!coverage.uncovered)
        return false;
    const BOOL enumerated = EnumDisplayMonitors(nullptr, nullptr,
        SubtractMonitor, reinterpret_cast<LPARAM>(&coverage));
    RECT remaining{};
    const bool covered = enumerated && coverage.valid &&
        GetRgnBox(coverage.uncovered, &remaining) == NULLREGION;
    DeleteObject(coverage.uncovered);
    return covered;
}

inline bool Unobscured(const Frame& frame) noexcept
{
    // Do not special-case our process, transparent/tool windows or disabled
    // windows: a floating Dock/backdrop/preview can still contribute pixels.
    // Rectangular rejection is deliberately conservative about window regions.
    // Bound the Z-order walk because another process can reorder/destroy HWNDs.
    std::size_t remaining = 4096;
    for (HWND other = GetWindow(frame.window, GW_HWNDPREV); other;)
    {
        if (remaining-- == 0 || other == frame.window)
            return false;
        const HWND previous = GetWindow(other, GW_HWNDPREV);
        if (IsWindowVisible(other) && !IsIconic(other))
        {
            DWORD cloaked = 0;
            const bool hidden = SUCCEEDED(DwmGetWindowAttribute(other,
                DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
            if (!hidden)
            {
                RECT bounds{}, intersection{};
                if (!GetWindowRect(other, &bounds) ||
                    IntersectRect(&intersection, &frame.sourceRect, &bounds))
                    return false;
            }
        }
        other = previous;
    }
    return true;
}

inline bool CaptureConditionsHold(const Frame& frame) noexcept
{
    return GetForegroundWindow() == frame.window && SameWindow(frame) &&
        SameVisibleGeometry(frame) && InsideVirtualScreen(frame.sourceRect) &&
        Unobscured(frame) && GetForegroundWindow() == frame.window &&
        SameWindow(frame) && SameVisibleGeometry(frame) &&
        GetForegroundWindow() == frame.window;
}

inline SIZE PixelSize(const RECT& rect) noexcept
{
    const auto width = static_cast<std::int64_t>(rect.right) - rect.left;
    const auto height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    if (width <= 0 || height <= 0 ||
        width > (std::numeric_limits<int>::max)() ||
        height > (std::numeric_limits<int>::max)())
        return {};
    constexpr double maximumEdge = 1600.0;
    constexpr double maximumPixels = 1600.0 * 1000.0;
    const double scale = (std::min)({1.0, maximumEdge / width,
        maximumEdge / height,
        std::sqrt(maximumPixels / (static_cast<double>(width) * height))});
    return {
        (std::max)(1L, static_cast<LONG>(std::floor(width * scale))),
        (std::max)(1L, static_cast<LONG>(std::floor(height * scale)))
    };
}

struct CaptureResources
{
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;

    CaptureResources() = default;
    CaptureResources(const CaptureResources&) = delete;
    CaptureResources& operator=(const CaptureResources&) = delete;
    ~CaptureResources() noexcept
    {
        if (memory && previousBitmap && previousBitmap != HGDI_ERROR)
            SelectObject(memory, previousBitmap);
        if (memory)
            DeleteDC(memory);
        if (bitmap)
            DeleteObject(bitmap);
        if (screen)
            ReleaseDC(nullptr, screen);
    }
};

inline bool Capture(const Request& request, Frame& result)
{
    ScopedPhysicalCoordinates coordinates;
    if (!coordinates || request.cancelled.load(std::memory_order_acquire))
        return false;
    result.window = request.frame.window;
    result.processId = request.frame.processId;
    result.threadId = request.frame.threadId;
    if (!SameWindow(result) || GetForegroundWindow() != result.window ||
        !ReadVisibleGeometry(result.window, result.sourceRect, result.placement))
        return false;
    result.pixelSize = PixelSize(result.sourceRect);
    if (result.pixelSize.cx <= 0 || result.pixelSize.cy <= 0 ||
        !CaptureConditionsHold(result))
        return false;

    CaptureResources resources;
    resources.screen = GetDC(nullptr);
    if (!resources.screen)
        return false;
    resources.memory = CreateCompatibleDC(resources.screen);
    if (!resources.memory)
        return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = result.pixelSize.cx;
    info.bmiHeader.biHeight = -result.pixelSize.cy;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    resources.bitmap = CreateDIBSection(resources.screen, &info,
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!resources.bitmap || !bits)
        return false;
    resources.previousBitmap = SelectObject(resources.memory, resources.bitmap);
    if (!resources.previousBitmap || resources.previousBitmap == HGDI_ERROR ||
        !SetStretchBltMode(resources.memory, HALFTONE) ||
        !SetBrushOrgEx(resources.memory, 0, 0, nullptr))
        return false;

    // Validate immediately around the only readback. No DwmFlush, window
    // activation, affinity changes, message sends or visibility changes occur.
    if (request.cancelled.load(std::memory_order_acquire) ||
        !CaptureConditionsHold(result) ||
        !StretchBlt(resources.memory, 0, 0,
            result.pixelSize.cx, result.pixelSize.cy, resources.screen,
            result.sourceRect.left, result.sourceRect.top,
            result.sourceRect.right - result.sourceRect.left,
            result.sourceRect.bottom - result.sourceRect.top,
            SRCCOPY | CAPTUREBLT) || !GdiFlush() ||
        request.cancelled.load(std::memory_order_acquire) ||
        !CaptureConditionsHold(result))
        return false;

    result.capturedTick = GetTickCount64();
    const auto pixelCount = static_cast<std::size_t>(result.pixelSize.cx) *
        static_cast<std::size_t>(result.pixelSize.cy);
    const auto* first = static_cast<const std::uint32_t*>(bits);
    result.pixels.assign(first, first + pixelCount);
    for (auto& pixel : result.pixels)
        pixel |= 0xff000000u;
    // A late minimize, move or overlay invalidates the whole attempt; never
    // replace a previous good cache entry with partially checked pixels.
    return !request.cancelled.load(std::memory_order_acquire) &&
        CaptureConditionsHold(result);
}

inline unsigned __stdcall Worker(void* parameter) noexcept
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::unique_ptr<std::shared_ptr<Request>> owner(
        static_cast<std::shared_ptr<Request>*>(parameter));
    auto request = std::move(*owner);
    owner.reset();
    try
    {
        Frame result;
        if (Capture(*request, result) &&
            !request->cancelled.load(std::memory_order_acquire))
            request->frame = std::move(result);
    }
    catch (...)
    {
        // Allocation/GDI failures are optional cache misses, not host errors.
    }
    request->ready.store(true, std::memory_order_release);
    return 0;
}
} // namespace detail

// Identity-only validation permits adoption after the window has minimized.
// Visible validation also checks physical geometry and normal/maximized state;
// callers decide separately whether they require the window to remain foreground.
inline bool IsCurrent(const Frame& frame, bool requireVisible) noexcept
{
    if (!detail::SameWindow(frame))
        return false;
    if (!requireVisible)
        return true;
    detail::ScopedPhysicalCoordinates coordinates;
    return coordinates && detail::SameVisibleGeometry(frame) &&
        detail::SameWindow(frame);
}

// The caller enforces its single-in-flight/refresh budget. Cancelling or dropping
// this request never blocks; the detached CRT worker retains only its own state.
// nullptr means request allocation failed. All returned requests eventually
// publish ready, including failure to start the worker.
inline std::shared_ptr<Request> Begin(HWND window) noexcept
{
    try
    {
        auto request = std::make_shared<Request>();
        request->frame.window = window;
        request->frame.threadId = GetWindowThreadProcessId(window,
            &request->frame.processId);
        if (!detail::SameWindow(request->frame) || GetForegroundWindow() != window)
        {
            request->ready.store(true, std::memory_order_release);
            return request;
        }
        auto* parameter = new (std::nothrow) std::shared_ptr<Request>(request);
        if (!parameter)
        {
            request->ready.store(true, std::memory_order_release);
            return request;
        }
        const auto thread = _beginthreadex(nullptr, 0, detail::Worker,
            parameter, 0, nullptr);
        if (!thread)
        {
            delete parameter;
            request->ready.store(true, std::memory_order_release);
        }
        else
            CloseHandle(reinterpret_cast<HANDLE>(thread));
        return request;
    }
    catch (...)
    {
        return nullptr;
    }
}
} // namespace snowdesktop::dock_snapshot_warmup
