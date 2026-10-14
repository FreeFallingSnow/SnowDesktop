#include "dock/dock_attraction_timeline.h"
#include "dock/dock_genie_rules.h"
#include "dock/dock_genie_timeline.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_shared_thumbnail.h"
#include "dock/dock_window_source_cloak.h"
#include "dock/dock_window_transition.h"
#include "settings/animation_settings.h"

#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <string>

struct DockWindowTransitionTestAccess
{
    static RECT AttractionHost(RECT window, RECT dock, std::vector<RECT> monitors)
    {
        DockWindowTransition transition;
        transition.effect_ = 4;
        transition.windowRect_ = window;
        transition.dockRect_ = dock;
        transition.animationMonitorRects_ = std::move(monitors);
        return transition.ResolveImageHostRect();
    }
    static void SetPresentationWait(DockWindowTransition& transition,
        std::function<HRESULT(IDCompositionDesktopDevice*)> wait)
    {
        transition.presentationWait_ = std::move(wait);
    }
    static void SetRetainedImageSize(DockWindowTransition& transition, SIZE size)
    {
        transition.compositionImageSize_ = size;
    }
    static RECT TargetBounds(const DockWindowTransition& transition)
    {
        return transition.windowRect_;
    }
    static bool RetainsPresentedImage(const DockWindowTransition& transition)
    {
        return transition.retiredWindowImage_.visual || !transition.retiredGenieWindowImages_.empty();
    }
    static bool TimelineActive(const DockWindowTransition& transition)
    {
        return transition.compositionTimelineActive_;
    }
    static void SeekProgress(DockWindowTransition& transition, double progress)
    {
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        transition.animationStartTimeMs_ = static_cast<double>(counter.QuadPart) * 1000.0 /
            static_cast<double>(frequency.QuadPart) - progress * transition.animationDurationMs_;
    }
    static double Collapse(const DockWindowTransition &transition)
    {
        return transition.lastCollapse_;
    }
    static bool HostContainsGenieOutline(const DockWindowTransition& transition)
    {
        using namespace snowdesktop::dock_genie;
        RECT host{};
        if (!GetWindowRect(transition.hwnd_, &host)) return false;
        const RECT window = transition.windowRect_, dock = transition.dockRect_;
        const Rect source{static_cast<double>(window.left), static_cast<double>(window.top),
            static_cast<double>(window.right), static_cast<double>(window.bottom)};
        const Rect target{static_cast<double>(dock.left), static_cast<double>(dock.top),
            static_cast<double>(dock.right), static_cast<double>(dock.bottom)};
        // Inspect the unchanged spatial mesh against the actual HWND, rather
        // than duplicating the host-size rule. Every reversed frame must fit.
        for (int phase = 0; phase <= 64; ++phase)
        {
            if (transition.effect_ == 4)
            {
                const auto patch = snowdesktop::dock_attraction::ControlPoints(
                    source, target, Mix(transition.collapseFrom_, transition.collapseTo_, phase / 64.0));
                for (int y = 0; y <= 16; ++y)
                    for (int x = 0; x <= 16; ++x)
                    {
                        const auto p = snowdesktop::dock_attraction::Surface(patch, x / 16.0, y / 16.0);
                        if (p.x < host.left - 0.5 || p.x > host.right + 0.5 || p.y < host.top - 0.5 ||
                            p.y > host.bottom + 0.5)
                            return false;
                    }
                continue;
            }
            for (std::size_t boundary = 0; boundary <= StripCount; ++boundary)
            {
                const auto section = SectionAt(source, target, transition.genieEdge_,
                    Mix(transition.collapseFrom_, transition.collapseTo_, phase / 64.0),
                    static_cast<double>(boundary) / StripCount);
                for (const double cross : {section.center - section.width * 0.5,
                        section.center + section.width * 0.5})
                {
                    const double x = Vertical(transition.genieEdge_) ? cross : section.axis;
                    const double y = Vertical(transition.genieEdge_) ? section.axis : cross;
                    if (x < host.left - 0.5 || x > host.right + 0.5 ||
                        y < host.top - 0.5 || y > host.bottom + 0.5)
                    {
                        std::cout << "Reversed Genie outside host: point=" << x << ',' << y
                            << " host=" << host.left << ',' << host.top << ',' << host.right
                            << ',' << host.bottom << '\n';
                        return false;
                    }
                }
            }
        }
        return true;
    }
    static HWND PresentDisplacedGenie(DockWindowTransition& transition,
        HWND source, RECT frame, RECT dock, snowdesktop::dock_genie::Edge edge,
        bool compositorTimeline)
    {
        transition.sourceWindow_ = source;
        transition.windowRect_ = frame;
        transition.dockRect_ = dock;
        transition.genieEdge_ = edge;
        transition.effect_ = 3;
        transition.imageHostRect_ = ResolveDockWindowImageHostRect(frame, dock);
        if (!transition.CreateSharedWindowImage()) return nullptr;
        const RECT host = transition.imageHostRect_;
        if (!SetWindowPos(transition.hwnd_, HWND_TOPMOST, host.left, host.top,
                host.right - host.left, host.bottom - host.top, SWP_NOACTIVATE))
            return nullptr;
        if (compositorTimeline)
        {
            // Hold the geometry at its displaced endpoint. Override only the
            // usual invisible endpoint opacity so its actual pixels can be read.
            transition.collapseFrom_ = transition.collapseTo_ = 1.0;
            if (!transition.StartGenieTimeline() ||
                FAILED(transition.compositionEffect_->SetOpacity(1.0f)) ||
                FAILED(transition.compositionDevice_->Commit())) return nullptr;
        }
        else if (!transition.ApplyGenieFrame(1.0, 255)) return nullptr;
        transition.presenting_ = true;
        ShowWindow(transition.hwnd_, SW_SHOWNOACTIVATE);
        return transition.hwnd_;
    }
    static HWND PresentAttraction(DockWindowTransition &transition, HWND source, RECT frame, RECT dock, bool timeline,
                                  double collapsed = 0.3)
    {
        transition.sourceWindow_ = source;
        transition.windowRect_ = frame;
        transition.dockRect_ = dock;
        transition.effect_ = 4;
        transition.imageHostRect_ = transition.ResolveImageHostRect();
        if (!transition.CreateSharedWindowImage())
            return nullptr;
        const RECT host = transition.imageHostRect_;
        if (!SetWindowPos(transition.hwnd_, HWND_TOPMOST, host.left, host.top, host.right - host.left,
                          host.bottom - host.top, SWP_NOACTIVATE))
            return nullptr;
        transition.collapseFrom_ = transition.collapseTo_ = collapsed;
        if (timeline ? !transition.StartAttractionTimeline() : !transition.ApplyAttractionFrame(collapsed, 255))
            return nullptr;
        if (timeline && collapsed > 0.9 &&
            (FAILED(transition.compositionEffect_->SetOpacity(1.0f)) || FAILED(transition.compositionDevice_->Commit())))
            return nullptr;
        transition.presenting_ = true;
        ShowWindow(transition.hwnd_, SW_SHOWNOACTIVATE);
        return transition.hwnd_;
    }
};

namespace
{
int failures = 0;
void Check(bool passed, const char* name)
{
    if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
struct Point { double x, y; };

struct ControlledPresentationWait
{
    const DWORD uiThread = GetCurrentThreadId();
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> calls{0};
    std::atomic<bool> ranOnUiThread{false};
    int blockedCall = 0;
    ~ControlledPresentationWait()
    {
        if (entered) CloseHandle(entered);
        if (release) CloseHandle(release);
        if (finished) CloseHandle(finished);
    }
    HRESULT Wait(IDCompositionDesktopDevice* device)
    {
        if (GetCurrentThreadId() == uiThread) ranOnUiThread.store(true);
        const bool first = calls.fetch_add(1) == blockedCall;
        HRESULT result = S_OK;
        if (first)
        {
            SetEvent(entered);
            // Simulate a delayed compositor, retaining the real commit/flush.
            // A blocking UI implementation cannot dispatch the release timer.
            if (WaitForSingleObject(release, 500) != WAIT_OBJECT_0) result = E_ABORT;
        }
        if (SUCCEEDED(result)) result = device->WaitForCommitCompletion();
        if (SUCCEEDED(result)) result = DwmFlush();
        if (first) SetEvent(finished);
        return result;
    }
};
Point Transform(const snowdesktop::dock_genie::ProjectiveMatrix& matrix, double x, double y)
{
    // Include the float conversion used by DirectComposition in the error bound.
    const double w = static_cast<float>(matrix.m14) * x +
        static_cast<float>(matrix.m24) * y + static_cast<float>(matrix.w);
    Check(w > 0 && std::isfinite(w), "strip has no perspective pole inside its clip");
    return {(static_cast<float>(matrix.m11) * x + static_cast<float>(matrix.m21) * y +
        static_cast<float>(matrix.dx)) / w,
        (static_cast<float>(matrix.m12) * x + static_cast<float>(matrix.m22) * y +
        static_cast<float>(matrix.dy)) / w};
}
double Gap(Point first, Point second)
{
    return std::hypot(first.x - second.x, first.y - second.y);
}

void CheckContinuousOutline()
{
    using namespace snowdesktop::dock_genie;
    constexpr Rect window{-1860, -750, 1980, 1410};
    constexpr Rect dock{-1700, 1480, -1636, 1544};
    bool affineStepsObserved = false;
    for (const auto edge : {Edge::Bottom, Edge::Top, Edge::Left, Edge::Right})
        for (const double collapsed : {0.0, 0.15, 0.35, 0.55, 0.8, 0.95, 1.0})
            for (std::size_t i = 0; i + 1 < StripCount; ++i)
            {
                const double begin = static_cast<double>(i) / StripCount;
                const double boundary = static_cast<double>(i + 1) / StripCount;
                const double end = static_cast<double>(i + 2) / StripCount;
                const auto first = StripProjectiveMatrix(window, dock, edge, collapsed,
                    3840, 2160, begin, boundary, -1920, -1080);
                const auto next = StripProjectiveMatrix(window, dock, edge, collapsed,
                    3840, 2160, boundary, end, -1920, -1080);
                for (const double cross : {0.0, 0.5, 1.0})
                {
                    const double x = (Vertical(edge) ? cross : boundary) * 3840;
                    const double y = (Vertical(edge) ? boundary : cross) * 2160;
                    Check(Gap(Transform(first, x, y), Transform(next, x, y)) < 0.15,
                        "neighbouring strip edges meet to subpixel precision at every animation phase");
                    const auto oldFirst = StripMatrix(window, dock, edge, collapsed,
                        3840, 2160, begin, boundary, -1920, -1080);
                    const auto oldNext = StripMatrix(window, dock, edge, collapsed,
                        3840, 2160, boundary, end, -1920, -1080);
                    const auto affine = [x, y](const Matrix& matrix) {
                        return Point{matrix.m11 * x + matrix.m21 * y + matrix.dx,
                            matrix.m12 * x + matrix.m22 * y + matrix.dy};
                    };
                    affineStepsObserved = affineStepsObserved || Gap(affine(oldFirst), affine(oldNext)) > 0.25;
                }
            }
    Check(affineStepsObserved, "continuity regression detects the previous affine staircase");
    for (const double collapsed : {0.0, 1.0})
    {
        const auto matrix = StripProjectiveMatrix(window, dock, Edge::Bottom, collapsed,
            3840, 2160, 0, 1, 0, 0);
        const Rect target = collapsed == 0 ? window : dock;
        Check(Gap(Transform(matrix, 0, 0), {target.left, target.top}) < 0.001 &&
            Gap(Transform(matrix, 3840, 2160), {target.right, target.bottom}) < 0.001,
            "projective animation preserves both window and Dock endpoints");
    }
}

void CheckTimelineGeometry()
{
    using namespace snowdesktop::dock_genie;
    double maximumError = 0, maximumGap = 0, maximumOpacityError = 0;
    bool positive = true;
    // Compare the compositor's float cubic coefficients with the exact spatial
    // deformation, including mid-segment times and reversals on mixed monitors.
    for (const bool croppedSource : {false, true})
    for (const double width : {1920.0, 3840.0, 7680.0})
        for (const auto edge : {Edge::Bottom, Edge::Top, Edge::Left, Edge::Right})
            for (const auto range : {std::array<double, 2>{0, 1}, {1, 0}, {0.43, 0}, {0.43, 1}})
            {
                const double height = width * 0.5625;
                const Rect window{-width, -height, 0, 0};
                const Rect dock{-width + 73, height * 0.3, -width + 137, height * 0.3 + 64};
                TimelineValues previous{};
                double previousSourceX = 0.0, previousSourceY = 0.0;
                for (std::size_t step = 0; step < TimelineSteps; ++step)
                    for (const double fraction : {0.0, 0.19, 0.51, 0.83, 1.0})
                        for (std::size_t strip = 0; strip < StripCount; ++strip)
                        {
                            const double begin = static_cast<double>(strip) / StripCount;
                            const double end = static_cast<double>(strip + 1) / StripCount;
                            const double sourceX = croppedSource && !Vertical(edge)
                                ? std::max(0.0, std::floor(begin * width - 0.75)) : 0.0;
                            const double sourceY = croppedSource && Vertical(edge)
                                ? std::max(0.0, std::floor(begin * height - 0.75)) : 0.0;
                            const auto sample = [&](std::size_t time) {
                                return SampleTimeline(window, dock, edge, range[0], range[1],
                                    static_cast<double>(time) / TimelineSteps, width, height,
                                    begin, end, -width, -height, sourceX, sourceY);
                            };
                            const auto first = sample(step), last = sample(step + 1);
                            TimelineValues value{};
                            for (std::size_t element = 0; element < value.size(); ++element)
                                value[element] = TimelineSegment(first, last, element, 0.08).Evaluate(
                                    fraction * 0.08 / TimelineSteps);
                            const double progress = (step + fraction) / TimelineSteps;
                            const double collapsed = Mix(range[0], range[1], Smooth(progress));
                            const auto exact = StripProjectiveMatrix(window, dock, edge, collapsed,
                                width, height, begin, end, -width, -height);
                            const auto project = [&](const TimelineValues& values, double axis, double cross,
                                double originX, double originY) {
                                const double x = (Vertical(edge) ? cross * width : axis * width) - originX;
                                const double y = (Vertical(edge) ? axis * height : cross * height) - originY;
                                const double w = values[3] * (Vertical(edge) ? y : x) + values[6];
                                positive = positive && w > 0 && std::isfinite(w);
                                return Point{(values[0] * x + (Vertical(edge) ? values[1] * y : 0) + values[4]) / w,
                                    ((Vertical(edge) ? 0 : values[1] * x) + values[2] * y + values[5]) / w};
                            };
                            for (const double axis : {begin, end})
                                for (const double cross : {0.0, 0.5, 1.0})
                                {
                                    const Point actual = project(value, axis, cross, sourceX, sourceY);
                                    const double x = (Vertical(edge) ? cross : axis) * width;
                                    const double y = (Vertical(edge) ? axis : cross) * height;
                                    maximumError = std::max(maximumError, Gap(actual, Transform(exact, x, y)));
                                }
                            if (strip)
                                for (const double cross : {0.0, 0.5, 1.0})
                                    maximumGap = std::max(maximumGap,
                                        Gap(project(previous, begin, cross, previousSourceX, previousSourceY),
                                            project(value, begin, cross, sourceX, sourceY)));
                            maximumOpacityError = std::max(maximumOpacityError, std::abs(value[7] - Opacity(collapsed)));
                            previous = value;
                            previousSourceX = sourceX;
                            previousSourceY = sourceY;
                        }
            }
    std::cout << "Genie cubic maximum error=" << maximumError << "px gap=" << maximumGap
        << "px opacity=" << maximumOpacityError << '\n';
    Check(positive, "compositor curves keep the entire mesh in front of the perspective camera");
    Check(maximumError < 0.5, "compositor timeline preserves the exact deformation to half a pixel through 8K");
    Check(maximumGap < 0.15, "adjacent compositor strips remain continuous between keyframes and during reversals");
    Check(maximumOpacityError < 1.0 / 255.0, "compositor opacity remains within one alpha step of the original effect");
}

void CheckAttractionGeometry()
{
    namespace a = snowdesktop::dock_attraction;
    const auto golden = a::ControlPoints({0, 0, 320, 200}, {336, 208, 400, 272}, 0.5);
    // Independently evaluated upstream top-left trajectory: p0=(0,0),
    // p1=(370.368,-25.4), p2=(395.93,228), p3=(368,240), parameter=0.5.
    Check(std::hypot(golden[0].x - 333.3605, golden[0].y - 105.97421875) < 0.001,
          "attraction retains the upstream asymmetric control-point trajectory");
    bool bounded = true, positive = true, curved = false;
    double maxCornerError = 0;
    for (const double width : {320.0, 2560.0, 7680.0})
    {
        const std::array<a::Rect, 4> targets{{{width + 30, width * 0.6, width + 94, width * 0.6 + 64},
                                              {-1600, 800, -1536, 864},
                                              {100, -800, 164, -736},
                                              {-900, -600, -836, -536}}};
        for (const auto target : targets)
        {
            const a::Rect window{0, 0, width, width * 0.625};
            const auto bounds = a::Bounds(window, target);
            for (int phase = 0; phase <= 192; ++phase)
            {
                const double collapse = phase / 192.0;
                const auto patch = a::ControlPoints(window, target, collapse);
                for (std::size_t i = 0; i < a::CellCount; ++i)
                {
                    const auto cell = a::SourceCell(i);
                    for (const double u : {cell.left, cell.right})
                        for (const double v : {cell.top, cell.bottom})
                        {
                            const auto p = a::Surface(patch, u, v);
                            bounded = bounded && p.x >= bounds.left && p.x <= bounds.right && p.y >= bounds.top &&
                                      p.y <= bounds.bottom;
                        }
                    const double cropX = std::max(0.0, std::floor(cell.left * width - 0.5));
                    const double cropY = std::max(0.0, std::floor(cell.top * width * 0.625 - 0.5));
                    const auto matrix =
                        a::Projection(patch, cell, width, width * 0.625, cropX, cropY, bounds.left, bounds.top);
                    if (matrix)
                        for (const double u : {cell.left, cell.right})
                            for (const double v : {cell.top, cell.bottom})
                            {
                                const double x = u * width - cropX, y = v * width * 0.625 - cropY;
                                const auto actual = Transform(*matrix, x, y);
                                const auto p = a::Surface(patch, u, v);
                                const double den = matrix->m14 * x + matrix->m24 * y + matrix->w;
                                positive = positive && den > 0 && std::isfinite(actual.x) && std::isfinite(actual.y);
                                maxCornerError = std::max(maxCornerError, std::hypot(actual.x - p.x + bounds.left,
                                                                                     actual.y - p.y + bounds.top));
                            }
                }
                const auto middle = a::Surface(patch, 0.5, 0.5);
                const auto affine = (patch[0] + patch[3] + patch[6] + patch[9]) * 0.25;
                curved = curved || std::hypot(middle.x - affine.x, middle.y - affine.y) > 2;
            }
            const auto first = a::ControlPoints(window, target, 0);
            Check(Gap({first[0].x, first[0].y}, {window.left, window.top}) < 0.001 &&
                      Gap({first[6].x, first[6].y}, {window.right, window.bottom}) < 0.001,
                  "attraction restore endpoint preserves the exact source frame");
        }
    }
    std::cout << "Attraction cropped projective corner error=" << maxCornerError << "px\n";
    Check(bounded, "complete attraction bounds contain sampled curves through 8K on both sides and above the source");
    Check(positive && maxCornerError < 0.5,
          "attraction cropped cells have finite positive perspective and subpixel corners");
    Check(curved && a::Opacity(0) == 1 && a::Opacity(1) == 0,
          "attraction deforms a curved patch and reaches a transparent collapsed endpoint");
    const a::Rect window{0, 0, 2560, 1600}, dock{2700, 800, 2764, 864};
    bool timelineFinite = true, timelineBounded = true;
    for (const auto range : {std::array<double, 2>{0, 1}, {0.6, 0}, {0.6, 1}})
    {
        std::array<a::Frame, a::TimelineSteps + 1> frames{};
        for (std::size_t step = 0; step < frames.size(); ++step)
            frames[step] =
                a::TimelineFrame(window, dock, range[0], range[1], static_cast<double>(step) / a::TimelineSteps);
        for (std::size_t i = 0; i < a::CellCount; ++i)
        {
            const auto cell = a::SourceCell(i);
            const double cropX = std::max(0.0, std::floor(cell.left * 2560 - 0.5));
            const double cropY = std::max(0.0, std::floor(cell.top * 1600 - 0.5));
            std::array<a::Values, a::TimelineSteps + 1> samples{};
            for (std::size_t step = 0; step < samples.size(); ++step)
                samples[step] = a::SampleCell(frames[step], window, dock, cell, 2560, 1600, cropX, cropY, -500, -500);
            a::StabilizeInactiveGeometry(samples);
            const auto tangents =
                a::CellTangents(samples, cell.left * 2560 - cropX - 0.5, cell.top * 1600 - cropY - 0.5,
                                cell.right * 2560 - cropX + 0.5, cell.bottom * 1600 - cropY + 0.5);
            for (std::size_t step = 0; step < a::TimelineSteps; ++step)
                for (const double fraction : {0.0, 0.19, 0.51, 0.83, 1.0})
                {
                    a::Values value{};
                    for (std::size_t element = 0; element < value.size(); ++element)
                        value[element] = a::TimelineSegment(samples[step], samples[step + 1], tangents[step],
                                                            tangents[step + 1], element, 0.42)
                                             .Evaluate(fraction * 0.42 / a::TimelineSteps);
                    for (const double u : {cell.left, cell.right})
                        for (const double v : {cell.top, cell.bottom})
                        {
                            const double x = u * 2560 - cropX + (u == cell.left ? -0.5 : 0.5);
                            const double y = v * 1600 - cropY + (v == cell.top ? -0.5 : 0.5);
                            const double w = value[2] * x + value[5] * y + value[8];
                            timelineFinite = timelineFinite && w > 0 &&
                                             std::isfinite((value[0] * x + value[3] * y + value[6]) / w) &&
                                             std::isfinite((value[1] * x + value[4] * y + value[7]) / w) &&
                                             value[9] >= -0.00001 && value[9] <= 1.00001 && value[10] >= -0.00001 &&
                                             value[10] <= 1.00001;
                            for (const int axis : {0, 1})
                            {
                                const auto project = [&](const a::Values &sample) {
                                    return (sample[axis] * x + sample[axis + 3] * y + sample[axis + 6]) /
                                           (sample[2] * x + sample[5] * y + sample[8]);
                                };
                                const double first = project(samples[step]), last = project(samples[step + 1]);
                                timelineBounded = timelineBounded && project(value) >= std::min(first, last) - 0.55 &&
                                                  project(value) <= std::max(first, last) + 0.55;
                            }
                        }
                }
        }
    }
    Check(timelineFinite,
          "actual float compositor curves retain finite positive perspective between samples and on reversals");
    Check(timelineBounded, "cropped corner playback cannot throw fragments beyond adjacent trajectory samples");
    const auto sourcePatch = a::ControlPoints(window, dock, 0);
    auto folded = sourcePatch;
    for (auto &point : folded)
        point.x = -point.x;
    const auto tiny = a::ControlPoints({0, 0, 2, 2}, {30, 30, 32, 32}, 0);
    Check(a::CellVisibility(sourcePatch, a::SourceCell(0)) == 1 && a::CellVisibility(folded, a::SourceCell(0)) == 0 &&
              a::CellVisibility(tiny, a::SourceCell(0)) < 0.001,
          "folded and subpixel slivers retire without revealing a finite endpoint remnant");
    const std::vector<RECT> monitors{{0, 0, 2560, 1600}, {2560, 510, 4480, 1590}};
    const RECT fullWindow{0, 0, 2560, 1600}, farDock{4400, 1450, 4464, 1514};
    const auto requested = DockWindowTransitionTestAccess::AttractionHost(fullWindow, farDock, monitors);
    const auto safe = ResolveDockWindowNonFullscreenHostRect(requested, monitors);
    Check(safe.has_value(), "maximized cross-screen attraction retains an eligible non-fullscreen host");
    if (safe)
    {
        bool covered = true;
        for (int phase = 0; phase <= 128; ++phase)
        {
            const auto patch = a::ControlPoints(window, {4400, 1450, 4464, 1514}, phase / 128.0);
            for (int y = 0; y <= 16; ++y)
                for (int x = 0; x <= 16; ++x)
                {
                    const auto point = a::Surface(patch, x / 16.0, y / 16.0);
                    const bool visible = std::any_of(monitors.begin(), monitors.end(), [&](RECT monitor) {
                        return point.x >= monitor.left && point.x <= monitor.right && point.y >= monitor.top &&
                               point.y <= monitor.bottom;
                    });
                    if (visible)
                        covered = covered && point.x >= safe->left - 1 && point.x <= safe->right + 1 &&
                                  point.y >= safe->top - 1 && point.y <= safe->bottom + 1;
                }
        }
        Check(covered, "both displays' visible attraction surface fits the host with its one-pixel Shell guard");
    }
}

LRESULT CALLBACK FrameFixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (GetWindowLongPtrW(window, GWLP_USERDATA))
        {
            RECT marker{20, 20, 36, 60};
            HBRUSH brush = CreateSolidBrush(RGB(220, 20, 40));
            FillRect(dc, &marker, brush);
            DeleteObject(brush);
            marker = {client.right - 36, 20, client.right - 20, 60};
            brush = CreateSolidBrush(RGB(20, 220, 40));
            FillRect(dc, &marker, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK MaximizedFixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETMINMAXINFO)
    {
        auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
        // Exercise a real WS_MAXIMIZE state without covering the user's desktop.
        info.ptMaxPosition = {-20000, -20000};
        info.ptMaxSize = {600, 420};
        info.ptMaxTrackSize = {1000, 800};
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::array<double, 2> FrameMarkerCenters(HWND ownedDestination)
{
    RECT area{};
    if (!GetWindowRect(ownedDestination, &area)) return {-1, -1};
    const int width = area.right - area.left, height = area.bottom - area.top;
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024)
        return {-1, -1};
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::array<double, 2> sums{}, counts{};
    if (bitmap)
    {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        // Capture only the rectangle occupied by this test's composition HWND
        // and opaque backing fixture; never inspect SnowDesktop or other apps.
        if (BitBlt(memory, 0, 0, width, height, screen, area.left, area.top, SRCCOPY | CAPTUREBLT))
        {
            const auto* pixels = static_cast<const unsigned*>(bits);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const unsigned pixel = pixels[y * width + x];
                    const unsigned red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
                    // The owned GDI markers can read back at R/G=122 after
                    // display colour conversion. Preserve their geometry using
                    // channel separation rather than a fixed brightness floor.
                    const bool redMarker = red > green + 40 && red > blue + 40;
                    const bool greenMarker = green > red + 40 && green > blue + 40;
                    if (redMarker)
                    {
                        sums[0] += x;
                        ++counts[0];
                    }
                    if (greenMarker)
                    {
                        sums[1] += x;
                        ++counts[1];
                    }
                }
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return {counts[0] ? sums[0] / counts[0] : -1, counts[1] ? sums[1] / counts[1] : -1};
}

void CheckFramePixelAlignment()
{
    using Microsoft::WRL::ComPtr;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopGenieFramePixelFixture";
    type.lpfnWndProc = FrameFixtureProc;
    Check(RegisterClassW(&type) != 0, "owned pixel-alignment fixture registers");
    const HWND source = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"Owned frame fixture", WS_OVERLAPPEDWINDOW, 500, 32, 320, 200,
        nullptr, nullptr, type.hInstance, nullptr);
    SetWindowLongPtrW(source, GWLP_USERDATA, 1);
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    DwmFlush();
    SIZE size{};
    RECT frame{}, client{};
    POINT clientOrigin{};
    HRESULT hr = snowdesktop::dock_thumbnail::SourceSize(source, size);
    if (SUCCEEDED(hr)) hr = DwmGetWindowAttribute(source, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
    const bool geometry = source && SUCCEEDED(hr) && GetClientRect(source, &client) && ClientToScreen(source, &clientOrigin);
    Check(geometry, "framed source supplies its visible bounds and client origin");
    HWND backing = nullptr, destination = nullptr;
    if (geometry)
    {
        const auto make = [&](DWORD extra) {
            return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | extra,
                type.lpszClassName, L"", WS_POPUP, 32, 32, size.cx, size.cy, nullptr, nullptr, type.hInstance, nullptr);
        };
        backing = make(0);
        destination = make(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED);
        SetLayeredWindowAttributes(destination, 0, 255, LWA_ALPHA);
        ShowWindow(backing, SW_SHOWNOACTIVATE);
        UpdateWindow(backing);
        ComPtr<ID3D11Device> graphics;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDCompositionDesktopDevice> composition;
        ComPtr<IDCompositionTarget> target;
        snowdesktop::dock_thumbnail::SharedVisual image;
        if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(&composition));
        if (SUCCEEDED(hr)) hr = image.Create(destination, source, composition.Get(), size);
        if (SUCCEEDED(hr)) hr = composition->CreateTargetForHwnd(destination, TRUE, &target);
        if (SUCCEEDED(hr)) hr = target->SetRoot(image.visual.Get());
        if (SUCCEEDED(hr)) hr = composition->Commit();
        if (SUCCEEDED(hr)) hr = composition->WaitForCommitCompletion();
        Check(SUCCEEDED(hr), "cropped shared frame is composed through the production image wrapper");
        if (SUCCEEDED(hr))
        {
            ShowWindow(destination, SW_SHOWNOACTIVATE);
            const ULONGLONG deadline = GetTickCount64() + 500;
            std::array<double, 2> actual{-1, -1};
            do
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                DwmFlush();
                actual = FrameMarkerCenters(destination);
                if (actual[0] >= 0 && actual[1] >= 0) break;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            } while (GetTickCount64() < deadline);
            const double red = clientOrigin.x - frame.left + 27.5;
            const double green = clientOrigin.x - frame.left + client.right - 28.5;
            std::cout << "Shared frame marker centers: " << actual[0] << ',' << actual[1]
                << " expected=" << red << ',' << green << '\n';
            Check(std::abs(actual[0] - red) < 1 && std::abs(actual[1] - green) < 1,
                "left and right image pixels align with the visible frame instead of its invisible resize border");
        }
        if (target) target->SetRoot(nullptr);
        if (composition) { composition->Commit(); composition->WaitForCommitCompletion(); }
        image.Reset();
        if (SUCCEEDED(hr))
        {
            snowdesktop::UiAnimationScheduler scheduler;
            DockWindowTransition transition;
            Check(scheduler.Initialize() && transition.Initialize(type.hInstance, &scheduler, nullptr, composition.Get()),
                "owned compositor animation fixture initializes");
            const double red = clientOrigin.x - frame.left + 27.5;
            const double green = clientOrigin.x - frame.left + client.right - 28.5;
            snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                2, 0, 60, false, false, 3);
            const RECT dock{frame.left + 80, frame.bottom + 8, frame.left + 144, frame.bottom + 72};
            const RECT host = ResolveDockWindowImageHostRect(frame, dock);
            ShowWindow(destination, SW_HIDE);
            SetWindowPos(backing, HWND_TOPMOST, host.left, host.top, host.right - host.left,
                host.bottom - host.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(backing);
            // A destination outside BOTH source axes makes a clip performed
            // after perspective discard the whole image. Read actual DWM
            // marker pixels, covering both live matrices and compositor curves.
            const RECT displacedDock{frame.right + 8, frame.bottom + 8,
                frame.right + 72, frame.bottom + 72};
            const RECT displacedHost = ResolveDockWindowImageHostRect(frame, displacedDock);
            SetWindowPos(backing, HWND_TOPMOST, displacedHost.left, displacedHost.top,
                displacedHost.right - displacedHost.left, displacedHost.bottom - displacedHost.top,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(backing);
            for (const auto edge : {snowdesktop::dock_genie::Edge::Bottom,
                    snowdesktop::dock_genie::Edge::Top, snowdesktop::dock_genie::Edge::Left,
                    snowdesktop::dock_genie::Edge::Right})
                for (const bool compositorTimeline : {false, true})
                {
                    DockWindowTransition displaced;
                    const bool initialized = displaced.Initialize(type.hInstance, nullptr, nullptr, composition.Get());
                    const HWND presented = initialized ? DockWindowTransitionTestAccess::PresentDisplacedGenie(
                        displaced, source, frame, displacedDock, edge, compositorTimeline) : nullptr;
                    Check(presented != nullptr, "displaced production Genie fixture prepares");
                    if (presented)
                    {
                        const std::array<double, 2> expected{
                            displacedDock.left - displacedHost.left + red * 64.0 / size.cx,
                            displacedDock.left - displacedHost.left + green * 64.0 / size.cx};
                        std::array<double, 2> actual{-1, -1};
                        const ULONGLONG deadline = GetTickCount64() + 500;
                        do
                        {
                            DwmFlush();
                            actual = FrameMarkerCenters(presented);
                            if (std::abs(actual[0] - expected[0]) < 2 &&
                                std::abs(actual[1] - expected[1]) < 2) break;
                        } while (GetTickCount64() < deadline);
                        std::cout << "Displaced Genie edge=" << static_cast<int>(edge)
                            << " timeline=" << compositorTimeline << " markers=" << actual[0]
                            << ',' << actual[1] << " expected=" << expected[0] << ',' << expected[1] << '\n';
                        Check(std::abs(actual[0] - expected[0]) < 2 &&
                            std::abs(actual[1] - expected[1]) < 2,
                            "source strip clips move with perspective beyond the original image bounds");
                    }
                    displaced.Cancel();
                }
            SetWindowPos(backing, HWND_TOPMOST, host.left, host.top, host.right - host.left,
                host.bottom - host.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(backing);
            const ULONGLONG prepareStarted = GetTickCount64();
            const bool started = transition.StartMinimize(source, displacedDock);
            std::cout << "Genie compositor preparation: " << GetTickCount64() - prepareStarted << "ms\n";
            Check(started && DockWindowTransitionTestAccess::TimelineActive(transition) &&
                scheduler.Metrics().activeAnimations == 0 && scheduler.Metrics().activeTimers == 1,
                "real Genie entry arms one completion timer without a UI frame animation");
            if (started)
            {
                ShowWindow(source, SW_MINIMIZE);
                const ULONGLONG deadline = GetTickCount64() + 250;
                bool moved = false;
                // Do not dispatch the animation scheduler: the test must observe
                // the real compositor moving the owned image while the UI stalls.
                do
                {
                    DwmFlush();
                    const auto markers = FrameMarkerCenters(transition.GetPresentationWindow());
                    if (markers[0] >= 0 && markers[1] >= 0 &&
                        markers[1] - markers[0] < client.right - 70)
                    {
                        moved = true;
                        break;
                    }
                } while (GetTickCount64() < deadline);
                Check(moved, "Genie window pixels deform while the UI animation scheduler is not dispatched");
                int requests = 0;
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.6);
                const auto restore = [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                    if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                    {
                        ++requests;
                        ShowWindow(window, SW_SHOWNOACTIVATE);
                    }
                };
                const bool reversed = transition.StartRestore(source, displacedDock, restore);
                Check(reversed && requests == 1,
                    "reversing a running compositor Genie requests native restore exactly once");
                const ULONGLONG preparationDeadline = GetTickCount64() + 1000;
                while (reversed && transition.IsActive() &&
                    !DockWindowTransitionTestAccess::TimelineActive(transition) && GetTickCount64() < preparationDeadline)
                {
                    scheduler.DispatchDue();
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    { TranslateMessage(&message); DispatchMessageW(&message); }
                    const HANDLE wake = scheduler.WaitHandle();
                    MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
                }
                Check(DockWindowTransitionTestAccess::TimelineActive(transition) &&
                    scheduler.Metrics().activeAnimations == 0 && requests == 1,
                    "reversed Genie returns to compositor playback after native restore preparation");
                Check(DockWindowTransitionTestAccess::HostContainsGenieOutline(transition),
                    "restoring a partially pulled Genie keeps its whole outline inside the render host");
                // Interrupt the reversal again before it completes. This must
                // retain the same full envelope through another native rebind.
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.25);
                Check(transition.StartMinimize(source, displacedDock),
                    "a prepared Genie restore can immediately reverse to minimize");
                ShowWindow(source, SW_MINIMIZE);
                Check(DockWindowTransitionTestAccess::HostContainsGenieOutline(transition),
                    "a repeated minimize reversal preserves the remaining deformed outline");
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.45);
                Check(transition.StartRestore(source, displacedDock, restore),
                    "a second interrupted Genie minimize requests another restore");
                const ULONGLONG secondDeadline = GetTickCount64() + 1000;
                while (transition.IsActive() && !DockWindowTransitionTestAccess::TimelineActive(transition) &&
                    GetTickCount64() < secondDeadline)
                {
                    scheduler.DispatchDue();
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    { TranslateMessage(&message); DispatchMessageW(&message); }
                    const HANDLE wake = scheduler.WaitHandle();
                    MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
                }
                Check(DockWindowTransitionTestAccess::TimelineActive(transition) && requests == 2 &&
                    DockWindowTransitionTestAccess::HostContainsGenieOutline(transition),
                    "repeated restore preparation preserves the entire remaining Genie silhouette");
                transition.Cancel();
                DWORD flags = 0;
                Check(!transition.IsActive() && scheduler.Metrics().activeAnimations == 0 &&
                    scheduler.Metrics().activeTimers == 0 &&
                    SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
                    (flags & DWM_CLOAKED_APP) == 0,
                    "cancelling a compositor Genie retires its wake and releases the source window");
            }
            // Exercise the new production mesh with actual live DWM pixels,
            // comparing CPU matrices and compositor property playback.
            for (const bool timeline : {false, true})
            {
                DockWindowTransition attraction;
                const bool initialized = attraction.Initialize(type.hInstance, nullptr, nullptr, composition.Get());
                const HWND presented = initialized ? DockWindowTransitionTestAccess::PresentAttraction(
                                                         attraction, source, frame, displacedDock, timeline)
                                                   : nullptr;
                Check(presented != nullptr, "owned attraction patch prepares its DWM source cells");
                if (presented)
                {
                    RECT hostBounds{};
                    GetWindowRect(presented, &hostBounds);
                    SetWindowPos(backing, presented, hostBounds.left, hostBounds.top,
                                 hostBounds.right - hostBounds.left, hostBounds.bottom - hostBounds.top,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    UpdateWindow(backing);
                    const auto patch = snowdesktop::dock_attraction::ControlPoints(
                        {static_cast<double>(frame.left), static_cast<double>(frame.top),
                         static_cast<double>(frame.right), static_cast<double>(frame.bottom)},
                        {static_cast<double>(displacedDock.left), static_cast<double>(displacedDock.top),
                         static_cast<double>(displacedDock.right), static_cast<double>(displacedDock.bottom)},
                        0.3);
                    const double v = (clientOrigin.y - frame.top + 39.5) / size.cy;
                    const auto p0 = snowdesktop::dock_attraction::Surface(patch, red / size.cx, v);
                    const auto p1 = snowdesktop::dock_attraction::Surface(patch, green / size.cx, v);
                    const std::array<double, 2> expected{p0.x - hostBounds.left, p1.x - hostBounds.left};
                    std::array<double, 2> actual{-1, -1};
                    const ULONGLONG deadline = GetTickCount64() + 500;
                    do
                    {
                        DwmFlush();
                        actual = FrameMarkerCenters(presented);
                        if (std::abs(actual[0] - expected[0]) < 4 && std::abs(actual[1] - expected[1]) < 4)
                            break;
                    } while (GetTickCount64() < deadline);
                    std::cout << "Attraction timeline=" << timeline << " markers=" << actual[0] << ',' << actual[1]
                              << " expected=" << expected[0] << ',' << expected[1] << '\n';
                    Check(std::abs(actual[0] - expected[0]) < 4 && std::abs(actual[1] - expected[1]) < 4,
                          "actual attraction image pixels follow the independent curved surface within the cell "
                          "sampling tolerance");
                }
                attraction.Cancel();
                const HWND tail = DockWindowTransitionTestAccess::PresentAttraction(attraction, source, frame,
                                                                                    displacedDock, timeline, 0.96);
                Check(tail != nullptr, "owned attraction tail prepares with finite geometry");
                if (tail)
                {
                    // Force the parent fully visible: cell retirement alone
                    // must suppress tiny end fragments, including GPU playback.
                    const ULONGLONG deadline = GetTickCount64() + 250;
                    std::array<double, 2> markers{};
                    do
                    {
                        DwmFlush();
                        markers = FrameMarkerCenters(tail);
                    } while ((markers[0] >= 0 || markers[1] >= 0) && GetTickCount64() < deadline);
                    Check(markers[0] < 0 && markers[1] < 0,
                          "a transparent subpixel tail cannot reappear as coloured source fragments");
                }
                attraction.Cancel();
            }
            snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn, 2, 0, 60, false, false, 4);
            ShowWindow(source, SW_SHOWNOACTIVATE);
            const ULONGLONG attractionStarted = GetTickCount64();
            const bool attractionStart = transition.StartMinimize(source, displacedDock);
            std::cout << "Attraction compositor preparation: " << GetTickCount64() - attractionStarted << "ms\n";
            Check(attractionStart && DockWindowTransitionTestAccess::TimelineActive(transition) &&
                      scheduler.Metrics().activeAnimations == 0 && scheduler.Metrics().activeTimers == 1,
                  "attraction uses compositor playback and one completion wake");
            if (attractionStart)
            {
                ShowWindow(source, SW_MINIMIZE);
                RECT hostBounds{};
                GetWindowRect(transition.GetPresentationWindow(), &hostBounds);
                const double initialRed = frame.left - hostBounds.left + red;
                const ULONGLONG movementDeadline = GetTickCount64() + 250;
                bool moved = false;
                do
                {
                    DwmFlush();
                    const auto centers = FrameMarkerCenters(transition.GetPresentationWindow());
                    if (centers[0] >= 0 && std::abs(centers[0] - initialRed) > 4)
                    {
                        moved = true;
                        break;
                    }
                } while (GetTickCount64() < movementDeadline);
                Check(moved, "actual attraction pixels keep moving while the UI animation scheduler is stalled");
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.55);
                int requests = 0;
                const auto restore = [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                    if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                    {
                        ++requests;
                        ShowWindow(window, SW_SHOWNOACTIVATE);
                    }
                };
                Check(transition.StartRestore(source, displacedDock, restore), "attraction reverses to restore");
                const double held = DockWindowTransitionTestAccess::Collapse(transition);
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.85);
                Check(transition.StartMinimize(source, displacedDock) &&
                          std::abs(held - DockWindowTransitionTestAccess::Collapse(transition)) < 0.0001,
                      "interrupting native restore preparation preserves the held deformation phase");
                ShowWindow(source, SW_MINIMIZE);
                DockWindowTransitionTestAccess::SeekProgress(transition, 0.45);
                Check(transition.StartRestore(source, displacedDock, restore),
                      "attraction repeatedly reverses to restore");
                const ULONGLONG deadline = GetTickCount64() + 1500;
                while (transition.IsActive() && !DockWindowTransitionTestAccess::TimelineActive(transition) &&
                       GetTickCount64() < deadline)
                {
                    scheduler.DispatchDue();
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    const HANDLE wake = scheduler.WaitHandle();
                    MsgWaitForMultipleObjects(1, &wake, FALSE, 10, QS_ALLINPUT);
                }
                Check(requests == 2 && DockWindowTransitionTestAccess::TimelineActive(transition) &&
                          DockWindowTransitionTestAccess::HostContainsGenieOutline(transition),
                      "repeated attraction restore rebind preserves the full curved render envelope");
                transition.Cancel();
                DWORD cloak = 0;
                Check(!transition.IsActive() && scheduler.Metrics().activeTimers == 0 &&
                          scheduler.Metrics().activeAnimations == 0 &&
                          SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloak, sizeof(cloak))) &&
                          (cloak & DWM_CLOAKED_APP) == 0,
                      "attraction cancellation releases its source image, wake and native cloak");
            }
        }
    }
    if (destination) DestroyWindow(destination);
    if (backing) DestroyWindow(backing);
    if (source) DestroyWindow(source);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
}

void CheckRetainedWindowImage()
{
    using Microsoft::WRL::ComPtr;
    using snowdesktop::dock_thumbnail::SharedVisual;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopGenieImageFixture";
    type.lpfnWndProc = DefWindowProcW;
    type.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    Check(RegisterClassW(&type) != 0, "owned image fixture class registers");
    const auto makeWindow = [&] {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            type.lpszClassName, L"", WS_POPUP, -20000, -20000, 320, 200,
            nullptr, nullptr, type.hInstance, nullptr);
    };
    // The source needs a normal redirection surface. Only the destination uses
    // NOREDIRECTIONBITMAP; both windows are owned fixtures outside the desktop.
    const HWND source = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"", WS_POPUP, -20000, -20000, 320, 200,
        nullptr, nullptr, type.hInstance, nullptr);
    const HWND destination = makeWindow();
    Check(source && destination, "owned source and destination fixtures are created");
    if (source && destination)
    {
        ComPtr<ID3D11Device> graphics;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDCompositionDesktopDevice> composition;
        if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(&composition));
        Check(SUCCEEDED(hr), "fixture composition device initializes");
        if (SUCCEEDED(hr))
        {
            ShowWindow(source, SW_SHOWNOACTIVATE);
            DwmFlush();
            SIZE visible{};
            hr = snowdesktop::dock_thumbnail::SourceSize(source, visible);
            std::cout << "DWM source size: " << visible.cx << 'x' << visible.cy << " hr=" << std::hex << hr << std::dec << '\n';
            Check(SUCCEEDED(hr) &&
                visible.cx == 320 && visible.cy == 200, "DWM supplies the current source image geometry");
            ShowWindow(source, SW_MINIMIZE);
            DwmFlush();
            SIZE minimized{};
            Check(IsIconic(source) && SUCCEEDED(snowdesktop::dock_thumbnail::SourceSize(source, minimized)) &&
                minimized.cx == visible.cx && minimized.cy == visible.cy,
                "DWM retains full window geometry without any SnowDesktop minimize cache");
            SharedVisual image;
            hr = image.Create(destination, source, composition.Get(), minimized);
            Check(SUCCEEDED(hr) && image.visual, "already minimized window produces a deformable shared image");
            if (SUCCEEDED(hr))
            {
                ComPtr<IDCompositionTarget> target;
                hr = composition->CreateTargetForHwnd(destination, TRUE, &target);
                if (SUCCEEDED(hr)) hr = target->SetRoot(image.visual.Get());
                const D2D_MATRIX_4X4_F identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                if (SUCCEEDED(hr)) hr = image.visual->SetTransform(identity);
                if (SUCCEEDED(hr)) hr = image.visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
                if (SUCCEEDED(hr)) hr = composition->Commit();
                if (SUCCEEDED(hr)) hr = composition->WaitForCommitCompletion();
                std::cout << "Shared image composition: hr=" << std::hex << hr << std::dec << '\n';
                Check(SUCCEEDED(hr), "shared source accepts projective transforms and soft edge rendering");
                if (target) target->SetRoot(nullptr);
                composition->Commit();
                composition->WaitForCommitCompletion();
            }
            image.Reset();
            Check(FAILED(image.Create(destination, nullptr, composition.Get(), minimized)) && !image.visual,
                "unavailable source fails cleanly for fallback");

            ComPtr<ID2D1Factory1> factory;
            ComPtr<ID2D1Device> d2d;
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) hr = factory->CreateDevice(dxgi.Get(), &d2d);
            snowdesktop::UiAnimationScheduler scheduler;
            Check(SUCCEEDED(hr) && scheduler.Initialize(), "owned animation fixture initializes");
            if (SUCCEEDED(hr) && scheduler.WaitHandle())
            {
                snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                    2, 0, 60, false, false, 3);
                for (const int effect : {1, 2, 3, 4})
                {
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    DockWindowTransition unavailable;
                    Check(unavailable.Initialize(type.hInstance, &scheduler, nullptr, nullptr),
                        "a missing shared-image device still leaves native window operations available");
                    std::wstring failurePresentation;
                    unavailable.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) failurePresentation = message;
                    });
                    const RECT fallbackDock{-19720, -19720, -19656, -19656};
                    ShowWindow(source, SW_SHOWNOACTIVATE);
                    Check(!unavailable.StartMinimize(source, fallbackDock) &&
                        unavailable.RequiresNativeAnimationFallback() && !unavailable.IsActive() &&
                        !unavailable.GetPresentationWindow() && !IsIconic(source) &&
                        failurePresentation.find(L"fallback=native-system-animation") != std::wstring::npos,
                        "shared-image failure requests native fallback for every selected effect");
                    DWORD cloakFlags = 0;
                    Check(SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloakFlags, sizeof(cloakFlags))) &&
                        !(cloakFlags & DWM_CLOAKED_APP), "failed animation leaves no source cloak or overlay");
                    ShowWindow(source, SW_MINIMIZE);
                    Check(IsIconic(source) != FALSE, "the original native minimize remains operational");
                    int unexpectedRestore = 0;
                    Check(!unavailable.StartRestore(source, fallbackDock,
                        [&](HWND, DockWindowRestoreTransitionPhase) { ++unexpectedRestore; }) &&
                        unavailable.RequiresNativeAnimationFallback() && !unavailable.IsActive() &&
                        unexpectedRestore == 0, "unavailable restore uses no previous minimize image or synthetic callback");
                    ShowWindow(source, SW_SHOWNOACTIVATE);
                    Check(!IsIconic(source), "native restore remains operational after shared-image failure");
                }
                snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                    2, 0, 60, false, false, 3);
                ShowWindow(source, SW_MINIMIZE);
                DwmFlush();
                DockWindowTransition transition;
                Check(transition.Initialize(type.hInstance, &scheduler, d2d.Get(), composition.Get()),
                    "fresh transition engine initializes without window snapshots");
                std::wstring presentation;
                transition.SetDiagnosticCallback([&](const wchar_t* message) {
                    if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                });
                const BOOL alreadyDisabled = TRUE;
                DwmSetWindowAttribute(source, DWMWA_TRANSITIONS_FORCEDISABLED,
                    &alreadyDisabled, sizeof(alreadyDisabled));
                const RECT dock{-19720, -19720, -19656, -19656};
                Check(SetWindowPos(destination, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
                    "owned Dock fixture starts its independently promoted session");
                int restores = 0;
                int activations = 0;
                bool requestedWhileCloaked = false;
                const bool restoring = transition.StartRestore(source, dock,
                    [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                        if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                        {
                            ++restores;
                            DWORD flags = 0;
                            requestedWhileCloaked = SUCCEEDED(DwmGetWindowAttribute(window,
                                DWMWA_CLOAKED, &flags, sizeof(flags))) && (flags & DWM_CLOAKED_APP) != 0;
                            ShowWindow(window, SW_SHOWNOACTIVATE);
                        }
                        else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored) ++activations;
                    }, destination);
                std::wcout << presentation << L'\n';
                Check(restoring && presentation.find(L"effective=3") != std::wstring::npos &&
                    presentation.find(L"image=dwm-shared-window") != std::wstring::npos,
                    "first restore uses Genie and a DWM image without a prior minimize animation");
                Check(restores == 1 && requestedWhileCloaked,
                    "restore is requested under the source cloak before the visual timeline, not after it");
                const auto drain = [&] {
                    const ULONGLONG deadline = GetTickCount64() + 3000;
                    while (transition.IsActive() && GetTickCount64() < deadline)
                    {
                        scheduler.DispatchDue();
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                        const HANDLE timer = scheduler.WaitHandle();
                        MsgWaitForMultipleObjects(1, &timer, FALSE, 10, QS_ALLINPUT);
                    }
                    Check(!transition.IsActive() && !transition.GetPresentationWindow(),
                        "animation completes and retires its entire presentation");
                    Check((GetWindowLongPtrW(destination, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                        "retiring a target animation keeps the independently promoted Dock topmost");
                };
                if (restoring) drain();
                Check(restores == 1 && !IsIconic(source), "restore handoff runs exactly once");
                Check(activations == 1, "restore handoff activates the completed source once");
                const auto appCloaked = [](HWND window) {
                    DWORD flags = 0;
                    const HRESULT query = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags));
                    Check(SUCCEEDED(query), "owned source cloak query succeeds");
                    return (flags & DWM_CLOAKED_APP) != 0;
                };
                Check(!appCloaked(source), "restore handoff releases only the animation's temporary source cloak");

                for (const int effect : {1, 2, 3, 4})
                {
                    std::cout << "Short-bar restore effect: " << effect << '\n';
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    // Emulate the short iconic bar reported for Electron: the
                    // native flag clears before the application's full geometry.
                    RECT fullRect{};
                    GetWindowRect(source, &fullRect);
                    ShowWindow(source, SW_MINIMIZE);
                    DwmFlush();
                    struct ShortRestore
                    {
                        DockWindowTransition* transition;
                        RECT fullRect;
                        bool prepared = false;
                        bool hiddenDuringBar = false;
                        bool waitingWhileHidden = false;
                        int requests = 0;
                        int activations = 0;
                        int imageFrames = 0;
                        int preparedFrames = 0;
                        double preparedAt = 0;
                        double activatedAt = 0;
                    } shortRestore{&transition, fullRect};
                    // A slow restored-image rebind must delay the beginning,
                    // rather than consume part of the visible animation. The
                    // third image preparation is the full-geometry rebind.
                    transition.SetOcclusionRectsProvider([&] {
                        if (++shortRestore.imageFrames == 3) Sleep(120);
                        if (shortRestore.prepared) ++shortRestore.preparedFrames;
                        return std::vector<RECT>{};
                    });
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock restore prepared:") == 0)
                        {
                            shortRestore.prepared = true;
                            shortRestore.preparedAt = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                        }
                    });
                    SetWindowLongPtrW(source, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&shortRestore));
                    Check(transition.StartRestore(source, dock,
                        [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                            if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                            {
                                ++shortRestore.requests;
                                ShowWindow(window, SW_SHOWNOACTIVATE);
                                SetWindowPos(window, nullptr, fullRect.left, fullRect.top, 160, 24,
                                    SWP_NOZORDER | SWP_NOACTIVATE);
                                Check(SetTimer(window, 2, 120, [](HWND targetWindow, UINT, UINT_PTR timer, DWORD) {
                                    KillTimer(targetWindow, timer);
                                    auto* state = reinterpret_cast<ShortRestore*>(GetWindowLongPtrW(targetWindow, GWLP_USERDATA));
                                    DWORD flags = 0;
                                    state->hiddenDuringBar = state->transition->IsActive() && !state->prepared &&
                                        !IsIconic(targetWindow) && SUCCEEDED(DwmGetWindowAttribute(targetWindow,
                                            DWMWA_CLOAKED, &flags, sizeof(flags))) && (flags & DWM_CLOAKED_APP) != 0;
                                    SetWindowPos(targetWindow, nullptr, state->fullRect.left, state->fullRect.top,
                                        state->fullRect.right - state->fullRect.left,
                                        state->fullRect.bottom - state->fullRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
                                    // Complete geometry can precede visibility too;
                                    // neither phase may start the visual timeline.
                                    ShowWindow(targetWindow, SW_HIDE);
                                    Check(SetTimer(targetWindow, 3, 80, [](HWND visibleWindow, UINT, UINT_PTR visibleTimer, DWORD) {
                                        KillTimer(visibleWindow, visibleTimer);
                                        auto* visibleState = reinterpret_cast<ShortRestore*>(GetWindowLongPtrW(visibleWindow, GWLP_USERDATA));
                                        DWORD cloakFlags = 0;
                                        RECT hiddenRect{};
                                        GetWindowRect(visibleWindow, &hiddenRect);
                                        visibleState->waitingWhileHidden = visibleState->transition->IsActive() &&
                                            !visibleState->prepared && !IsWindowVisible(visibleWindow) &&
                                            EqualRect(&hiddenRect, &visibleState->fullRect) &&
                                            SUCCEEDED(DwmGetWindowAttribute(visibleWindow, DWMWA_CLOAKED,
                                                &cloakFlags, sizeof(cloakFlags))) && (cloakFlags & DWM_CLOAKED_APP) != 0;
                                        ShowWindow(visibleWindow, SW_SHOWNOACTIVATE);
                                    }) != 0, "owned hidden restore visibility timer is armed");
                                }) != 0, "owned short-bar restore completion timer is armed");
                            }
                            else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored)
                            {
                                ++shortRestore.activations;
                                shortRestore.activatedAt = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                                RECT actual{};
                                GetWindowRect(window, &actual);
                                Check(EqualRect(&actual, &fullRect) && IsWindowVisible(window) && !appCloaked(window),
                                    "source is exposed only at its complete geometry");
                            }
                        }), "owned short-bar restore starts");
                    Check(shortRestore.requests == 1, "short-bar fixture receives the restore immediately");
                    drain();
                    Check(shortRestore.hiddenDuringBar && shortRestore.waitingWhileHidden && shortRestore.prepared &&
                        shortRestore.requests == 1 && shortRestore.activations == 1 && !appCloaked(source),
                        "full geometry and visibility are both required before any restore effect starts or retires");
                    const double visibleDuration = (effect == 3 ? 360.0 : effect == 2 ? 180.0 : 240.0) *
                        snowdesktop::animation::RuntimeDurationScale();
                    Check(shortRestore.activatedAt - shortRestore.preparedAt >= visibleDuration - 15.0,
                        "slow restored-image preparation does not skip the start of the visible animation");
                    Check(shortRestore.preparedFrames <= 4,
                        "all restore effects advance on the compositor without CPU frame submissions");
                    transition.SetOcclusionRectsProvider({});
                    KillTimer(source, 2);
                    KillTimer(source, 3);
                    SetWindowLongPtrW(source, GWLP_USERDATA, 0);
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                    });
                }
                const auto pumpUntil = [&](const auto& predicate, ULONGLONG timeout) {
                    const ULONGLONG deadline = GetTickCount64() + timeout;
                    while (!predicate() && GetTickCount64() < deadline)
                    {
                        scheduler.DispatchDue();
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                        const HANDLE timer = scheduler.WaitHandle();
                        MsgWaitForMultipleObjects(1, &timer, FALSE, 10, QS_ALLINPUT);
                    }
                    return predicate();
                };
                // The Win10 capture keeps invisible resize borders while iconic.
                // Supply that platform-specific retained size at the real restore
                // entry; HWND restore, image rebind and scheduler remain real.
                const HWND framedSource = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, -20000, -20000, 600, 420,
                    nullptr, nullptr, type.hInstance, nullptr);
                Check(framedSource != nullptr, "owned framed restore fixture initializes");
                if (framedSource)
                {
                    DwmSetWindowAttribute(framedSource, DWMWA_TRANSITIONS_FORCEDISABLED,
                        &alreadyDisabled, sizeof(alreadyDisabled));
                    ShowWindow(framedSource, SW_SHOWNOACTIVATE);
                    DwmFlush();
                    RECT legacy{}, visibleFrame{};
                    GetWindowRect(framedSource, &legacy);
                    Check(SUCCEEDED(DwmGetWindowAttribute(framedSource, DWMWA_EXTENDED_FRAME_BOUNDS,
                        &visibleFrame, sizeof(visibleFrame))) &&
                        (legacy.right - legacy.left != visibleFrame.right - visibleFrame.left ||
                            legacy.bottom - legacy.top != visibleFrame.bottom - visibleFrame.top),
                        "framed fixture distinguishes invisible borders from the visible image");
                    for (const int effect : {1, 2, 3, 4})
                    {
                        for (const int sizeCase : {0, 1, 2})
                        {
                            snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                                2, 0, 60, false, false, effect);
                            ShowWindow(framedSource, SW_MINIMIZE);
                            DwmFlush();
                            int requested = 0, activated = 0, fallback = 0;
                            bool prepared = false;
                            bool resized = false;
                            snowdesktop::UiScheduleToken resizeToken = 0;
                            double preparedAt = 0, activatedAt = 0;
                            transition.SetDiagnosticCallback([&](const wchar_t* message) {
                                if (std::wstring(message).find(L"Dock restore prepared:") == 0)
                                {
                                    prepared = true;
                                    preparedAt = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                                    if (sizeCase == 2)
                                    {
                                        resizeToken = scheduler.ScheduleOnce(80, [&](snowdesktop::UiScheduleToken) {
                                            resized = SetWindowPos(framedSource, nullptr, -20011, -20009,
                                                707, 493, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                                        });
                                    }
                                }
                            });
                            const bool started = transition.StartRestore(framedSource, dock,
                                [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                                    if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                                    {
                                        ++requested;
                                        ShowWindow(window, SW_SHOWNOACTIVATE);
                                    }
                                    else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored)
                                    {
                                        ++activated;
                                        activatedAt = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
                                        if (sizeCase == 2)
                                        {
                                            RECT current{};
                                            DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &current, sizeof(current));
                                            const RECT target = DockWindowTransitionTestAccess::TargetBounds(transition);
                                            Check(resized && EqualRect(&current, &target),
                                                "late native geometry update aligns the final image before source exposure");
                                        }
                                    }
                                    else ++fallback;
                                });
                            Check(started, "framed restore starts through the production entry");
                            DockWindowTransitionTestAccess::SetRetainedImageSize(transition,
                                sizeCase == 1 ? SIZE{737, 511} : SIZE{legacy.right - legacy.left, legacy.bottom - legacy.top});
                            if (started) drain();
                            if (resizeToken) scheduler.Cancel(resizeToken);
                            const double visibleDuration = (effect == 3 ? 360.0 : effect == 2 ? 180.0 : 240.0) *
                                snowdesktop::animation::RuntimeDurationScale();
                            Check(prepared && requested == 1 && activated == 1 && fallback == 0 &&
                                !IsIconic(framedSource) && !appCloaked(framedSource),
                                "retained border or size mismatch rebinds the live image instead of skipping restore");
                            Check(prepared && activatedAt - preparedAt >= visibleDuration - 15.0,
                                "size-mismatched restore plays its complete visible timeline");
                            Check(prepared && activatedAt - preparedAt < visibleDuration + 400.0,
                                "late size changes do not consume the restore cleanup timeout");
                            SetWindowPos(framedSource, nullptr, legacy.left, legacy.top,
                                legacy.right - legacy.left, legacy.bottom - legacy.top, SWP_NOZORDER | SWP_NOACTIVATE);
                            DwmFlush();
                        }
                    }
                    DestroyWindow(framedSource);
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                    });
                }
                WNDCLASSW maximizedType{};
                maximizedType.hInstance = type.hInstance;
                maximizedType.lpszClassName = L"SnowDesktopMaximizedRestoreFixture";
                maximizedType.lpfnWndProc = MaximizedFixtureProc;
                maximizedType.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
                Check(RegisterClassW(&maximizedType) != 0, "owned maximized restore fixture registers");
                const HWND maximizedSource = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                    maximizedType.lpszClassName, L"", WS_OVERLAPPEDWINDOW, -20000, -20000, 600, 420,
                    nullptr, nullptr, type.hInstance, nullptr);
                Check(maximizedSource != nullptr, "owned maximized restore fixture initializes");
                if (maximizedSource)
                {
                    DwmSetWindowAttribute(maximizedSource, DWMWA_TRANSITIONS_FORCEDISABLED,
                        &alreadyDisabled, sizeof(alreadyDisabled));
                    for (const int effect : {1, 2, 3, 4})
                    {
                        snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                            2, 0, 60, false, false, effect);
                        ShowWindow(maximizedSource, SW_SHOWMAXIMIZED);
                        RECT maximizedBounds{};
                        GetWindowRect(maximizedSource, &maximizedBounds);
                        Check(IsZoomed(maximizedSource) && maximizedBounds.right < -19000,
                            "native maximized fixture stays outside the user's desktop");
                        ShowWindow(maximizedSource, SW_MINIMIZE);
                        DwmFlush();
                        auto wait = std::make_shared<ControlledPresentationWait>();
                        wait->blockedCall = 1; // Acknowledge preparation, then delay late image replacement.
                        DockWindowTransitionTestAccess::SetPresentationWait(transition,
                            [wait](IDCompositionDesktopDevice* device) { return wait->Wait(device); });
                        int requested = 0, activated = 0, fallback = 0;
                        bool resized = false;
                        RECT presentedHost{};
                        snowdesktop::UiScheduleToken resizeToken = 0;
                        transition.SetDiagnosticCallback([&](const wchar_t* message) {
                            if (std::wstring(message).find(L"Dock restore prepared:") == 0)
                            {
                                GetWindowRect(transition.GetPresentationWindow(), &presentedHost);
                                resizeToken = scheduler.ScheduleOnce(80, [&](snowdesktop::UiScheduleToken) {
                                    RECT actual{};
                                    GetWindowRect(maximizedSource, &actual);
                                    resized = SetWindowPos(maximizedSource, nullptr, actual.left, actual.top + 9,
                                        actual.right - actual.left, actual.bottom - actual.top - 9,
                                        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                                });
                            }
                        });
                        Check(transition.StartRestore(maximizedSource, dock,
                            [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                                if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                                {
                                    ++requested;
                                    ShowWindow(window, SW_SHOWMAXIMIZED);
                                }
                                else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored) ++activated;
                                else ++fallback;
                            }), "maximized restore starts through the production entry");
                        Check(pumpUntil([&] { return WaitForSingleObject(wait->entered, 0) == WAIT_OBJECT_0; }, 1200),
                            "late maximized frame replacement reaches its presentation barrier");
                        RECT currentHost{};
                        GetWindowRect(transition.GetPresentationWindow(), &currentHost);
                        Check(resized && IsZoomed(maximizedSource) && transition.IsActive() &&
                            appCloaked(maximizedSource) && activated == 0,
                            "late replacement keeps the maximized source cloaked until its image is presented");
                        Check(DockWindowTransitionTestAccess::RetainsPresentedImage(transition),
                            "late replacement retains the previous live DWM relationships during the pending commit");
                        Check(EqualRect(&presentedHost, &currentHost),
                            "a late maximized border shrink does not move or shrink the presented host");
                        scheduler.ScheduleOnce(80, [wait](snowdesktop::UiScheduleToken) { SetEvent(wait->release); });
                        drain();
                        if (resizeToken) scheduler.Cancel(resizeToken);
                        Check(requested == 1 && activated == 1 && fallback == 0 && !IsIconic(maximizedSource) &&
                            !appCloaked(maximizedSource) && wait->calls.load() == 4 && !wait->ranOnUiThread.load(),
                            "maximized restore acknowledges replacement before source exposure and retires cleanly");
                        Check(!DockWindowTransitionTestAccess::RetainsPresentedImage(transition),
                            "completed maximized handoff releases its retired live relationships");
                        DockWindowTransitionTestAccess::SetPresentationWait(transition, {});
                    }
                    DestroyWindow(maximizedSource);
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                    });
                }
                // Real restore entry with a controlled slow presentation wait:
                // the source stays cloaked while independent UI work progresses.
                for (const int effect : {1, 2, 3, 4})
                {
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    ShowWindow(source, SW_MINIMIZE);
                    DwmFlush();
                    auto wait = std::make_shared<ControlledPresentationWait>();
                    Check(wait->entered && wait->release && wait->finished, "controlled presentation events initialize");
                    DockWindowTransitionTestAccess::SetPresentationWait(transition,
                        [wait](IDCompositionDesktopDevice* device) { return wait->Wait(device); });
                    int heartbeats = 0;
                    const auto heartbeat = scheduler.ScheduleInterval(8, [&](snowdesktop::UiScheduleToken) {
                        if (WaitForSingleObject(wait->entered, 0) == WAIT_OBJECT_0 &&
                            WaitForSingleObject(wait->finished, 0) == WAIT_TIMEOUT)
                        {
                            ++heartbeats;
                            composition->Commit(); // Independent desktop/host work remains runnable.
                        }
                    });
                    int requested = 0, activated = 0, fallback = 0;
                    Check(transition.StartRestore(source, dock,
                        [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                            if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                            {
                                ++requested;
                                ShowWindow(window, SW_SHOWNOACTIVATE);
                            }
                            else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored) ++activated;
                            else ++fallback;
                        }), "slow-presentation restore starts through the production entry");
                    Check(pumpUntil([&] { return WaitForSingleObject(wait->entered, 0) == WAIT_OBJECT_0; }, 1000),
                        "restore reaches its controlled compositor wait");
                    Check(transition.IsActive() && appCloaked(source) && activated == 0,
                        "pending presentation retains source suppression and the transition");
                    scheduler.ScheduleOnce(80, [wait](snowdesktop::UiScheduleToken) { SetEvent(wait->release); });
                    drain();
                    scheduler.Cancel(heartbeat);
                    Check(heartbeats >= 3 && !wait->ranOnUiThread.load(),
                        "UI deadlines advance while all restore presentation waits run off the UI thread");
                    Check(requested == 1 && activated == 1 && fallback == 0 && !appCloaked(source) &&
                        wait->calls.load() == 3,
                        "prepared image, exposed source and transparent overlay are acknowledged before restore retirement");
                    DockWindowTransitionTestAccess::SetPresentationWait(transition, {});
                }
                // Cancel and destroy the owner while a wait is outstanding. Its
                // worker must own only its device/result, never a host callback.
                ShowWindow(source, SW_MINIMIZE);
                DwmFlush();
                auto cancellationWait = std::make_shared<ControlledPresentationWait>();
                auto cancelled = std::make_unique<DockWindowTransition>();
                Check(cancelled->Initialize(type.hInstance, &scheduler, d2d.Get(), composition.Get()),
                    "owned pending-wait cancellation fixture initializes");
                DockWindowTransitionTestAccess::SetPresentationWait(*cancelled,
                    [cancellationWait](IDCompositionDesktopDevice* device) { return cancellationWait->Wait(device); });
                int cancelledActivations = 0;
                Check(cancelled->StartRestore(source, dock,
                    [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                        if (phase == DockWindowRestoreTransitionPhase::RequestRestore) ShowWindow(window, SW_SHOWNOACTIVATE);
                        else ++cancelledActivations;
                    }), "owned cancellation restore starts");
                Check(pumpUntil([&] { return WaitForSingleObject(cancellationWait->entered, 0) == WAIT_OBJECT_0; }, 1000),
                    "cancellation fixture reaches its pending presentation");
                cancelled->Cancel();
                cancelled.reset();
                Check(!appCloaked(source), "cancelling a pending presentation releases its temporary source cloak");
                SetEvent(cancellationWait->release);
                Check(pumpUntil([&] { return WaitForSingleObject(cancellationWait->finished, 0) == WAIT_OBJECT_0; }, 1000) &&
                    cancelledActivations == 0, "outstanding wait completes safely after owner destruction without activation");
                ShowWindow(source, SW_MINIMIZE);
                DwmFlush();
                DockWindowTransitionTestAccess::SetPresentationWait(transition,
                    [](IDCompositionDesktopDevice*) { return E_FAIL; });
                int failedFallback = 0;
                Check(transition.StartRestore(source, dock,
                    [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                        if (phase == DockWindowRestoreTransitionPhase::RequestRestore) ShowWindow(window, SW_SHOWNOACTIVATE);
                        else if (phase == DockWindowRestoreTransitionPhase::FallbackWithoutAnimation) ++failedFallback;
                    }), "owned failed-presentation restore starts");
                drain();
                Check(failedFallback == 1 && !appCloaked(source) && !IsIconic(source),
                    "failed presentation falls back once without leaving a hidden source or stale overlay");
                DockWindowTransitionTestAccess::SetPresentationWait(transition, {});
                snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                    2, 0, 60, false, false, 3);
                const DWORD preparationStart = GetTickCount();
                const bool minimizing = transition.StartExternalMinimize(source, dock,
                    preparationStart + snowdesktop::dock_minimize::kRequestTimeoutMs, destination);
                Check(minimizing && presentation.find(L"image=dwm-shared-window") != std::wstring::npos,
                    "native minimize prepares a shared Genie image within the hook deadline");
                std::cout << "Shared Genie preparation: " << GetTickCount() - preparationStart << " ms\n";
                if (minimizing)
                {
                    Check(appCloaked(source), "native minimize keeps the real source out of the composition scene");
                    ShowWindow(source, SW_MINIMIZE);
                    drain();
                    Check(IsIconic(source) && !appCloaked(source), "minimize remains committed after scene retirement and source release");
                }
                // Model a toolkit UI thread which dispatches the native state
                // change after the visual timeline. The source must remain
                // cloaked then, otherwise a second native animation can flash.
                for (const int effect : {1, 2, 3, 4})
                {
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    ShowWindow(source, SW_SHOWNOACTIVATE);
                    const BOOL enabled = FALSE;
                    DwmSetWindowAttribute(source, DWMWA_TRANSITIONS_FORCEDISABLED, &enabled, sizeof(enabled));
                    struct LateMinimize { DockWindowTransition* transition; bool retained = false; } late{&transition};
                    SetWindowLongPtrW(source, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&late));
                    const bool delayed = transition.StartMinimize(source, dock);
                    Check(delayed, "Dock minimize begins for a delayed toolkit state change");
                    if (delayed)
                    {
                        const double duration = effect == 3 ? 360.0 : effect == 2 ? 180.0 : 240.0;
                        const UINT delay = static_cast<UINT>(duration * snowdesktop::animation::RuntimeDurationScale() + 200.0);
                        Check(SetTimer(source, 1, delay, [](HWND window, UINT, UINT_PTR timer, DWORD) {
                            KillTimer(window, timer);
                            auto* state = reinterpret_cast<LateMinimize*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                            DWORD flags = 0;
                            state->retained = state->transition->IsActive() &&
                                SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
                                (flags & DWM_CLOAKED_APP) != 0;
                            ShowWindow(window, SW_MINIMIZE);
                        }) != 0, "owned delayed state change timer is armed");
                        drain();
                        Check(late.retained && IsIconic(source),
                            "minimize timeline retains native suppression until the delayed application commits its minimized state");
                        Check(!appCloaked(source), "delayed minimize releases its temporary source cloak after handoff");
                        KillTimer(source, 1);
                    }
                    SetWindowLongPtrW(source, GWLP_USERDATA, 0);
                }
                ShowWindow(source, SW_SHOWNOACTIVATE);
                Check(transition.StartMinimize(source, dock), "owned cancel fixture starts its transition");
                transition.Cancel();
                Check(!appCloaked(source) && !IsIconic(source), "cancelling an animation immediately reveals its unminimized source");
                const BOOL cloaked = TRUE;
                DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
                Check(transition.StartMinimize(source, dock), "a precloaked source retains a DWM composition image");
                transition.Cancel();
                Check(appCloaked(source), "animation does not release an application cloak it did not acquire");
                const BOOL uncloaked = FALSE;
                DwmSetWindowAttribute(source, DWMWA_CLOAK, &uncloaked, sizeof(uncloaked));
                bool acquired = false;
                Check(FAILED(snowdesktop::dock_source_cloak::Acquire(source, nullptr, acquired)) &&
                    !acquired && !appCloaked(source), "an invalid animation owner cannot leave a hidden source behind");
                const HWND leaseOwner = makeWindow();
                Check(leaseOwner && snowdesktop::dock_source_cloak::RegisterOwner(leaseOwner),
                    "owned crash recovery fixture establishes its owner generation");
                Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(source, leaseOwner, acquired)) && acquired,
                    "source cloak records ownership before hiding the window");
                Check(!snowdesktop::dock_source_cloak::RecoverStale(source) && appCloaked(source),
                    "discovery preserves a live animation owner");
                Check(snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source, DWM_CLOAKED_APP) == 0 &&
                    snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source,
                        DWM_CLOAKED_APP | DWM_CLOAKED_SHELL) == DWM_CLOAKED_SHELL &&
                    snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source,
                        DWM_CLOAKED_APP | DWM_CLOAKED_INHERITED) == DWM_CLOAKED_INHERITED,
                    "task discovery retains an animating app while preserving other desktop and inherited exclusions");
                DestroyWindow(leaseOwner);
                Check(snowdesktop::dock_source_cloak::RecoverStale(source) && !appCloaked(source),
                    "discovery reveals an existing app after its animation owner disappears");
                const HWND reusedOwner = makeWindow();
                Check(reusedOwner && snowdesktop::dock_source_cloak::RegisterOwner(reusedOwner),
                    "reused owner fixture establishes a new generation");
                Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(source, reusedOwner, acquired)) && acquired,
                    "second recovery fixture acquires its source");
                Check(snowdesktop::dock_source_cloak::RegisterOwner(reusedOwner) &&
                    snowdesktop::dock_source_cloak::RecoverStale(source) && !appCloaked(source),
                    "a different owner generation cannot retain a stale source cloak despite a live recycled handle");
                DestroyWindow(reusedOwner);
                Check(snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source, DWM_CLOAKED_APP) == DWM_CLOAKED_APP,
                    "an application's own unmarked cloak still excludes it from Dock task discovery");
            }
        }
    }
    if (source) DestroyWindow(source);
    if (destination) DestroyWindow(destination);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
}
}

int main()
{
    // Preserve minimize protocol boundaries without installing cross-process Hooks.
    using namespace snowdesktop::dock_minimize;
    Check(RequestIsCurrent(100, 0), "fresh request is current");
    Check(!RequestIsCurrent(100, 100), "expired request is rejected");
    Check(!RequestIsCurrent(100, 101), "late request is rejected");
    Check(RequestIsCurrent(50, MAXDWORD - 100), "request remains valid across tick wrap");
    Check(!RequestIsCurrent(1000, 0), "future request outside budget is rejected");
    Check(IsMinimizeCommand(SW_MINIMIZE) && IsMinimizeCommand(SW_SHOWMINIMIZED) &&
        IsMinimizeCommand(SW_SHOWMINNOACTIVE) && IsMinimizeCommand(SW_FORCEMINIMIZE),
        "all native minimize forms are recognized");
    Check(!IsMinimizeCommand(SW_RESTORE) && !IsMinimizeCommand(SW_MAXIMIZE) &&
        !IsMinimizeCommand(SW_HIDE), "restore, maximize and tray hiding remain independent");
    CheckContinuousOutline();
    CheckTimelineGeometry();
    CheckAttractionGeometry();
    CheckFramePixelAlignment();
    CheckRetainedWindowImage();
    if (!failures) std::cout << "Genie outline, minimize protocol and cache-independent DWM image checks passed\n";
    return failures ? 1 : 0;
}
