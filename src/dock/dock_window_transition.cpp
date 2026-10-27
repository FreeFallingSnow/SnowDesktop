#include "dock_window_transition.h"
#include "diagnostics/performance_trace.h"
#include "dock_attraction_timeline.h"
#include "dock_genie_timeline.h"
#include "dock_minimize_protocol.h"
#include "dock_window_rules.h"
#include "dock_window_source_cloak.h"
#include "platform/shell_overlay_window.h"
#include "settings/animation_settings.h"
#include "platform/window_support/window_support_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <new>

namespace
{

constexpr wchar_t kDockWindowTransitionClassName[] =
    L"SnowDesktopDockWindowTransition";
std::atomic<unsigned> presentationWaitJobs{0};
std::atomic<unsigned> sceneRetirementJobs{0};
constexpr UINT kRetireSceneWindow = WM_APP + 51;

BOOL CALLBACK CollectAnimationMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return FALSE;
    try { reinterpret_cast<std::vector<RECT>*>(parameter)->push_back(info.rcMonitor); }
    catch (...) { return FALSE; }
    return TRUE;
}

bool IsUsableRect(const RECT& rect)
{
    return rect.right - rect.left > 1 &&
        rect.bottom - rect.top > 1;
}

snowdesktop::dock_genie::Rect DeformationRect(RECT rect) noexcept
{
    return {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right),
            static_cast<double>(rect.bottom)};
}

struct GenieSourceBand
{
    D2D1_RECT_F clip;
    RECT crop;
};

GenieSourceBand ResolveGenieSourceBand(SIZE size, snowdesktop::dock_genie::Edge edge, std::size_t index,
                                       bool attraction = false) noexcept
{
    const float width = static_cast<float>(size.cx), height = static_cast<float>(size.cy);
    const float begin = static_cast<float>(index) / snowdesktop::dock_genie::StripCount;
    const float end = static_cast<float>(index + 1) / snowdesktop::dock_genie::StripCount;
    // Preserve the fractional overlap, with integer crops providing its
    // bilinear sampling neighbours and no change to the image's pixel scale.
    const auto cell = snowdesktop::dock_attraction::SourceCell(index);
    const D2D1_RECT_F clip =
        attraction ? D2D1::RectF(std::max(0.0f, static_cast<float>(cell.left) * width - 0.5f),
                                 std::max(0.0f, static_cast<float>(cell.top) * height - 0.5f),
                                 std::min(width, static_cast<float>(cell.right) * width + 0.5f),
                                 std::min(height, static_cast<float>(cell.bottom) * height + 0.5f))
        : snowdesktop::dock_genie::Vertical(edge)
            ? D2D1::RectF(0, std::max(0.0f, begin * height - 0.75f), width, std::min(height, end * height + 0.75f))
            : D2D1::RectF(std::max(0.0f, begin * width - 0.75f), 0, std::min(width, end * width + 0.75f), height);
    RECT crop{static_cast<LONG>(std::floor(clip.left)), static_cast<LONG>(std::floor(clip.top)),
        static_cast<LONG>(std::ceil(clip.right)), static_cast<LONG>(std::ceil(clip.bottom))};
    // The shared-image API requires dimensions above one pixel, even when
    // an individual band of a tiny source window is subpixel-sized.
    crop.right = std::min(size.cx, std::max(crop.right, crop.left + 2));
    crop.bottom = std::min(size.cy, std::max(crop.bottom, crop.top + 2));
    crop.left = std::min(crop.left, crop.right - 2);
    crop.top = std::min(crop.top, crop.bottom - 2);
    return {clip, crop};
}

bool SystemWindowAnimationsEnabled()
{
    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) ||
        !compositionEnabled)
        return false;

    return snowdesktop::animation::RuntimeAnimationsEnabled() &&
        snowdesktop::animation::RuntimeWindowEffect() != 0;
}

double TransitionDuration(int effect) noexcept
{
    return (effect == 4   ? 420.0
            : effect == 3 ? 360.0
            : effect == 2 ? 180.0
                          : 240.0) *
           snowdesktop::animation::RuntimeDurationScale();
}

double MonotonicTimeMilliseconds() noexcept
{
    LARGE_INTEGER counter{};
    static const double ticksPerMillisecond = [] {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0)
            return 0.0;
        return static_cast<double>(
            frequency.QuadPart) / 1000.0;
    }();
    if (!QueryPerformanceCounter(&counter) ||
        ticksPerMillisecond <= 0.0)
    {
        return static_cast<double>(GetTickCount64());
    }
    return static_cast<double>(
        counter.QuadPart) / ticksPerMillisecond;
}

HRGN CreateDockWindowTransitionRegion(
    const RECT& bounds, int cornerRadius)
{
    if (cornerRadius <= 0)
    {
        return CreateRectRgn(
            bounds.left, bounds.top,
            bounds.right, bounds.bottom);
    }
    const int diameter = cornerRadius * 2;
    return CreateRoundRectRgn(
        bounds.left, bounds.top,
        bounds.right, bounds.bottom,
        diameter, diameter);
}

} // namespace

struct DockWindowTransition::PresentationBarrier
{
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> device;
    std::function<HRESULT(IDCompositionDesktopDevice*)> wait;
    bool minimize = false;
    std::atomic<bool> completed{false};
    HRESULT result = E_PENDING;
};

// Reserve the complete retirement owner before publishing a scene. No
// allocation or fallible thread-pool submission is needed during cancellation.
struct DockWindowTransition::RetiredScene
{
    std::atomic<unsigned> references{1};
    std::atomic<bool> windowDestroyed{false};
    std::atomic<bool> completed{false};
    PTP_WORK work = nullptr;
    HWND window = nullptr;
    HANDLE token = nullptr;
    HWND source = nullptr;
    snowdesktop::dock_source_cloak::Owner owner;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> device;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scale;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    Microsoft::WRL::ComPtr<IDCompositionRectangleClip> clip;
    std::vector<Microsoft::WRL::ComPtr<IDCompositionVisual3>> strips;
    std::vector<Microsoft::WRL::ComPtr<IDCompositionVisual2>> triangles;
    snowdesktop::dock_thumbnail::SharedVisual image, previousImage;
    std::vector<snowdesktop::dock_thumbnail::SharedVisual> images, previousImages;
    std::function<HRESULT(IDCompositionDesktopDevice*)> wait;
    std::function<BOOL(HWND, UINT, WPARAM, LPARAM)> post;

    void ReleaseResources() noexcept
    {
        // Thumbnail unregistration and COM release belong to this worker too.
        // Keep the resource budget until those releases have finished.
        image.Reset(); previousImage.Reset();
        images.clear(); previousImages.clear();
        strips.clear(); triangles.clear();
        clip.Reset(); effect.Reset(); scale.Reset(); visual.Reset();
        target.Reset(); device.Reset();
    }
    void Release() noexcept
    {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
    ~RetiredScene()
    {
        ReleaseResources();
        if (work) CloseThreadpoolWork(work);
        sceneRetirementJobs.fetch_sub(1, std::memory_order_relaxed);
    }
};

DockWindowTransition::DockWindowTransition() = default;

bool DockWindowTransition::ReserveSceneRetirement()
{
    if (sceneRetirement_) return true;
    if (sceneRetirementJobs.fetch_add(1, std::memory_order_relaxed) >= 4)
    {
        sceneRetirementJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    auto state = std::unique_ptr<RetiredScene>(new (std::nothrow) RetiredScene());
    if (!state)
    {
        sceneRetirementJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    try { state->wait = cleanupWait_; state->post = retirementPost_; }
    catch (...) { return false; }
    state->work = CreateThreadpoolWork(RunRetiredScene, state.get(), nullptr);
    if (!state->work) return false;
    sceneRetirement_ = std::move(state);
    return true;
}

void CALLBACK DockWindowTransition::RunRetiredScene(
    PTP_CALLBACK_INSTANCE instance, void* context, PTP_WORK)
{
    CallbackMayRunLong(instance);
    auto* state = static_cast<RetiredScene*>(context);
    try
    {
        if (state->wait) state->wait(state->device.Get());
        else if (state->device && SUCCEEDED(state->device->WaitForCommitCompletion())) DwmFlush();
    }
    catch (...) { /* A failed acknowledgement still retires this hidden scene. */ }
    // The source-thread recovery timer can finish a timed-out release while
    // composition is stalled. Otherwise retain this exact old lease and retry
    // away from the UI. A newer owner is never released by this completion.
    while (state->source && IsWindow(state->source) &&
        snowdesktop::dock_source_cloak::SameOwner(state->source, state->owner))
    {
        if (SUCCEEDED(snowdesktop::dock_source_cloak::Release(state->source, state->owner))) break;
        Sleep(100);
    }
    state->ReleaseResources();
    state->completed.store(true, std::memory_order_release);
    // The HWND owns a second reference and keeps its budget until NCDESTROY.
    // Failed notification still has this worker owner and can be retried.
    while (state->window && !state->windowDestroyed.load(std::memory_order_acquire))
    {
        BOOL posted = FALSE;
        try
        {
            posted = state->post ? state->post(state->window, kRetireSceneWindow,
                reinterpret_cast<WPARAM>(state->token), reinterpret_cast<LPARAM>(state)) :
                PostMessageW(state->window, kRetireSceneWindow,
                    reinterpret_cast<WPARAM>(state->token), reinterpret_cast<LPARAM>(state));
        }
        catch (...) {}
        if (posted) break;
        Sleep(30);
    }
    state->Release();
}

void CALLBACK DockWindowTransition::RunPresentationBarrier(
    PTP_CALLBACK_INSTANCE instance, void* context)
{
    if (instance) CallbackMayRunLong(instance);
    std::unique_ptr<std::shared_ptr<PresentationBarrier>> work(
        static_cast<std::shared_ptr<PresentationBarrier>*>(context));
    auto& state = **work;
    {
        snowdesktop::performance::Scope performanceScope("dock.transition", "presentation.wait");
        try
        {
            if (state.wait) state.result = state.wait(state.device.Get());
            else
            {
                state.result = state.device->WaitForCommitCompletion();
                // Preserve each direction's acknowledgement contract. Minimize
                // uses DWM only as the original failed-commit fallback.
                if (state.minimize ? FAILED(state.result) : SUCCEEDED(state.result))
                    state.result = DwmFlush();
            }
        }
        catch (...) { state.result = E_FAIL; }
    }
    // This work owns the device and result only. Cancellation or destruction
    // never leaves a worker with a transition, HWND or host callback to touch.
    state.completed.store(true, std::memory_order_release);
    presentationWaitJobs.fetch_sub(1, std::memory_order_relaxed);
}

bool DockWindowTransition::BeginPresentationBarrier(RestorePresentationPhase phase)
{
    if (!compositionDevice_) return false;
    // Bound outstanding waits even if DWM stalls and the user cancels/restarts.
    if (presentationWaitJobs.fetch_add(1, std::memory_order_relaxed) >= 4)
    {
        presentationWaitJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    std::shared_ptr<PresentationBarrier> state;
    try
    {
        state = std::make_shared<PresentationBarrier>();
        state->device = compositionDevice_;
        state->wait = presentationWait_;
        state->minimize = awaitingMinimizePresentation_;
    }
    catch (...)
    {
        presentationWaitJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    auto* work = new (std::nothrow) std::shared_ptr<PresentationBarrier>(state);
    if (!work)
    {
        presentationWaitJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    const BOOL queued = TrySubmitThreadpoolCallback(RunPresentationBarrier, work, nullptr);
    if (!queued)
    {
        delete work;
        presentationWaitJobs.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    presentationBarrier_ = std::move(state);
    restorePresentationPhase_ = phase;
    presentationBarrierDeadlineMs_ = MonotonicTimeMilliseconds() + kRestoreCleanupTimeoutMs;
    return true;
}

std::optional<HRESULT> DockWindowTransition::PresentationBarrierResult(double now) const
{
    if (!presentationBarrier_) return E_UNEXPECTED;
    if (presentationBarrier_->completed.load(std::memory_order_acquire))
        return presentationBarrier_->result;
    if (now >= presentationBarrierDeadlineMs_)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return std::nullopt;
}

HRGN CreateDockWindowTransitionOcclusionRegion(
    const RECT& hostBounds, int cornerRadius,
    const std::vector<RECT>& occluders)
{
    const RECT localBounds{0, 0,
        hostBounds.right - hostBounds.left,
        hostBounds.bottom - hostBounds.top};
    HRGN region = CreateDockWindowTransitionRegion(localBounds, cornerRadius);
    if (!region)
        return nullptr;
    for (const RECT& occluder : occluders)
    {
        RECT intersection{};
        if (!IntersectRect(&intersection, &hostBounds, &occluder))
            continue;
        OffsetRect(&intersection, -hostBounds.left, -hostBounds.top);
        HRGN excluded = CreateRectRgnIndirect(&intersection);
        const bool succeeded = excluded &&
            CombineRgn(region, region, excluded, RGN_DIFF) != ERROR;
        if (excluded)
            DeleteObject(excluded);
        if (!succeeded)
        {
            DeleteObject(region);
            return nullptr;
        }
    }
    return region;
}

bool DockWindowTransition::ApplyOcclusion(
    const RECT& hostBounds, int cornerRadius)
{
    const auto occluders = occlusionRectsProvider_
        ? occlusionRectsProvider_() : std::vector<RECT>{};
    if (hasOcclusionRegion_ && cornerRadius == occlusionCornerRadius_ &&
        EqualRect(&hostBounds, &occlusionHostBounds_) &&
        occluders.size() == occlusionRects_.size() &&
        std::equal(occluders.begin(), occluders.end(), occlusionRects_.begin(),
            [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; }))
        return true;

    HRGN region = CreateDockWindowTransitionOcclusionRegion(
        hostBounds, cornerRadius, occluders);
    if (!region)
        return false;
    if (!SetWindowRgn(hwnd_, region, FALSE))
    {
        DeleteObject(region);
        return false;
    }
    occlusionHostBounds_ = hostBounds;
    occlusionCornerRadius_ = cornerRadius;
    occlusionRects_ = occluders;
    hasOcclusionRegion_ = true;
    return true;
}

void DockWindowTransition::RefreshOcclusion()
{
    if (!presenting_ || !hwnd_)
        return;
    const RECT bounds = UsesCompositionImage()
        ? imageHostRect_ : lastFrameRect_;
    const int radius = UsesCompositionImage()
        ? 0 : ResolveDockWindowTransitionCornerRadius(bounds, dockRect_);
    if (!ApplyOcclusion(bounds, radius))
        CompleteImmediately();
}

void DockWindowTransition::LogPresentation(int requestedEffect,
    const wchar_t* fallbackStage)
{
    if (!diagnosticCallback_)
        return;
    wchar_t message[512]{};
    swprintf_s(message,
        L"Dock transition: hwnd=%p direction=%ls requested=%d effective=%d "
        L"image=%ls fallback=%ls result=0x%08lX",
        static_cast<void*>(sourceWindow_),
        direction_ == DockWindowTransitionDirection::Minimize ? L"minimize" : L"restore",
        requestedEffect, effect_, imageSource_,
        fallbackStage, static_cast<unsigned long>(imageResult_));
    diagnosticCallback_(message);
}

double EaseDockWindowTransition(double progress) noexcept
{
    progress = std::clamp(progress, 0.0, 1.0);
    return progress * progress * (3.0 - 2.0 * progress);
}

BYTE ResolveDockWindowTransitionOpacity(
    DockWindowTransitionDirection direction,
    double progress) noexcept
{
    const double eased =
        EaseDockWindowTransition(progress);
    const double opacity =
        direction ==
            DockWindowTransitionDirection::Minimize
        ? 255.0 * (1.0 - eased)
        : 255.0 * eased;
    return static_cast<BYTE>(std::clamp(
        static_cast<int>(std::lround(opacity)),
        0, 255));
}

int ResolveDockWindowTransitionCornerRadius(
    const RECT& frame,
    const RECT& dockRect) noexcept
{
    const int frameShortSide = std::min(
        std::max(0L, frame.right - frame.left),
        std::max(0L, frame.bottom - frame.top));
    const int dockShortSide = std::min(
        std::max(0L, dockRect.right - dockRect.left),
        std::max(0L, dockRect.bottom - dockRect.top));
    if (frameShortSide <= 1 ||
        dockShortSide <= 1)
        return 0;

    // Deriving the radius from the Dock target makes the mask naturally follow
    // the monitor DPI because Dock geometry is already expressed in physical
    // pixels. Keep tiny targets useful and guard against malformed giant ones.
    const int desiredRadius = std::clamp(
        static_cast<int>(std::lround(
            dockShortSide * 0.18)),
        4, 48);
    return std::min(
        desiredRadius,
        frameShortSide / 2);
}

RECT InterpolateDockWindowTransitionRect(
    const RECT& from, const RECT& to, double progress) noexcept
{
    const double eased = EaseDockWindowTransition(progress);
    const auto interpolate = [eased](LONG start, LONG end) {
        return static_cast<LONG>(std::lround(
            start + (end - start) * eased));
    };
    RECT result{
        interpolate(from.left, to.left),
        interpolate(from.top, to.top),
        interpolate(from.right, to.right),
        interpolate(from.bottom, to.bottom)
    };
    if (result.right <= result.left)
        result.right = result.left + 1;
    if (result.bottom <= result.top)
        result.bottom = result.top + 1;
    return result;
}

RECT ResolveDockWindowImageHostRect(
    const RECT& from, const RECT& to) noexcept
{
    return {
        std::min(from.left, to.left),
        std::min(from.top, to.top),
        std::max(from.right, to.right),
        std::max(from.bottom, to.bottom)
    };
}

std::optional<RECT> ResolveDockWindowNonFullscreenHostRect(RECT host, std::span<const RECT> monitors) noexcept
{
    const auto valid = [](RECT rect) { return rect.right > rect.left && rect.bottom > rect.top; };
    if (!valid(host) || monitors.empty())
        return std::nullopt;
    const auto covers = [](RECT rect, RECT monitor) {
        return rect.left <= monitor.left && rect.top <= monitor.top && rect.right >= monitor.right &&
               rect.bottom >= monitor.bottom;
    };
    bool fullscreen = false;
    for (const RECT monitor : monitors)
    {
        if (!valid(monitor))
            return std::nullopt;
        if (!covers(host, monitor))
            continue;
        fullscreen = true;
    }
    if (!fullscreen)
        return host;
    std::optional<RECT> best;
    std::uint64_t largestArea = 0;
    // Offset displays may need different guarded edges. A single edge cannot
    // uncover both monitors; still remove at most one pixel from any edge.
    for (unsigned edges = 1; edges < 16; ++edges)
    {
        const RECT candidate{host.left + ((edges & 1u) ? 1 : 0), host.top + ((edges & 2u) ? 1 : 0),
                             host.right - ((edges & 4u) ? 1 : 0), host.bottom - ((edges & 8u) ? 1 : 0)};
        if (!valid(candidate))
            continue;
        if (std::any_of(monitors.begin(), monitors.end(), [&](RECT monitor) { return covers(candidate, monitor); }))
            continue;
        const auto area = static_cast<std::uint64_t>(static_cast<std::int64_t>(candidate.right) - candidate.left) *
                          static_cast<std::uint64_t>(static_cast<std::int64_t>(candidate.bottom) - candidate.top);
        if (area > largestArea)
        {
            best = candidate;
            largestArea = area;
        }
    }
    return best;
}

DockWindowTransition::~DockWindowTransition()
{
    if (animationScheduler_ && minimizeObservationToken_)
        animationScheduler_->Cancel(minimizeObservationToken_);
    pendingMinimizes_.Clear();
    Cancel();
    if (compositionTarget_)
        compositionTarget_->SetRoot(nullptr);
    compositionClip_.Reset();
    compositionEffect_.Reset();
    compositionScaleTransform_.Reset();
    compositionVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    if (hwnd_)
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(
            kDockWindowTransitionClassName, instance_);
}

bool DockWindowTransition::Initialize(
    HINSTANCE instance,
    snowdesktop::UiAnimationScheduler* animationScheduler,
    ID2D1Device*,
    IDCompositionDesktopDevice* compositionDevice)
{
    instance_ = instance;
    animationScheduler_ = animationScheduler;
    // Keep transition transactions and completion waits independent of the
    // host's widget/desktop device. This scene uses shared visuals only and
    // needs no additional rendering surface or screenshot resource.
    compositionDevice_.Reset();
    if (compositionDevice)
        DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&compositionDevice_));
    const bool registered = RegisterWindowClass();
    if (registered) EnsureWindow();
    return registered;
}

bool DockWindowTransition::RegisterWindowClass()
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.cbWndExtra = sizeof(LONG_PTR);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName =
        kDockWindowTransitionClassName;
    return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool DockWindowTransition::EnsureWindow()
{
    if (hwnd_ && IsWindow(hwnd_))
        return true;
    if (!instance_)
        return false;
    // Another initialized transition can retire its last HWND and unregister
    // the shared class while this object temporarily has no presentation HWND.
    if (!RegisterWindowClass()) return false;

    hwnd_ = snowdesktop::CreateShellOverlayWindowEx(
        kDockWindowTransitionExStyle,
        kDockWindowTransitionClassName,
        L"Dock Window Transition",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;
    if (!snowdesktop::dock_source_cloak::RegisterOwner(hwnd_))
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    const DWM_WINDOW_CORNER_PREFERENCE corner =
        kDockWindowTransitionCornerPreference;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));
    const DWMNCRENDERINGPOLICY ncRendering =
        kDockWindowTransitionNcRenderingPolicy;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_NCRENDERING_POLICY,
        &ncRendering, sizeof(ncRendering));
    const COLORREF borderColor =
        kDockWindowTransitionBorderColor;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_BORDER_COLOR,
        &borderColor, sizeof(borderColor));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(
        hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions,
        sizeof(disableTransitions));
    return true;
}

struct DockWindowTransition::RemoteAnimation
{
    HWND source = nullptr;
    DockWindowTransitionDirection direction = DockWindowTransitionDirection::Minimize;
    bool pending = true;
    ULONGLONG acceptedAt = GetTickCount64();
};
bool DockWindowTransition::StartHelperAnimation(HWND source, RECT anchor, HWND below, bool restore, RestoreCallback callback)
{
    namespace support = snowdesktop::window_support;
    namespace wire = support::protocol;
    auto& runtime = support::WindowSupport();
    if (!helperRouting_ || runtime.Resolve(support::Capability::MinimizeAnimation, source).provider != support::Provider::Helper)
        return false;
    if (IsActive() && !remoteAnimation_) Cancel();
    auto state = std::make_shared<RemoteAnimation>();
    state->source = source;
    state->direction = restore ? DockWindowTransitionDirection::Restore : DockWindowTransitionDirection::Minimize;
    wire::Request request;
    request.command = restore ? wire::Command::StartRestore : wire::Command::StartMinimize;
    request.target = wire::Capture(source);
    request.animation = {request.target, anchor, reinterpret_cast<UINT_PTR>(below),
        static_cast<std::uint32_t>(snowdesktop::animation::RuntimeWindowEffect()),
        snowdesktop::animation::RuntimeAnimationsEnabled() ? 1u : 0u,
        static_cast<std::uint32_t>(snowdesktop::animation::detail::speed.load()),
        static_cast<std::uint32_t>(snowdesktop::animation::detail::frameLimit.load()),
        static_cast<std::uint32_t>(snowdesktop::animation::detail::dockPosition.load())};
    if (!runtime.Submit(request, [weak = std::weak_ptr<RemoteAnimation>(state), callback = std::move(callback), identity = request.target](const auto& response) {
        const auto active = weak.lock();
        if (!active) return;
        active->pending = false;
        if (callback && wire::Current(identity))
            callback(wire::Window(identity), response.succeeded ? DockWindowRestoreTransitionPhase::ActivateRestored :
                DockWindowRestoreTransitionPhase::FallbackWithoutAnimation);
    })) return false;
    remoteAnimation_ = std::move(state);
    ObserveMinimize(source, anchor, true);
    return true;
}
bool DockWindowTransition::StartMinimize(
    HWND sourceWindow, RECT dockRect,
    HWND keepBelowWindow, std::function<void(HWND)> minimizeRequest)
{
    if (StartHelperAnimation(sourceWindow, dockRect, keepBelowWindow, false)) return true;
    ObserveMinimize(sourceWindow, dockRect, true);
    const bool alreadyPending = IsActiveFor(sourceWindow) && (awaitingMinimizePresentation_ || externalMinimize_);
    const bool started = Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Minimize,
        {}, keepBelowWindow);
    if (started && minimizeRequest && !alreadyPending)
    {
        if (awaitingMinimizePresentation_) minimizeRequest_ = std::move(minimizeRequest);
        else if (SourceIdentityCurrent()) minimizeRequest(sourceWindow_);
    }
    return started;
}

void DockWindowTransition::SetMinimizeCompletionCallbacks(MinimizeTargetProvider target,
    std::function<void(const MinimizeTarget&)> completed)
{
    if (animationScheduler_ && minimizeObservationToken_)
        animationScheduler_->Cancel(minimizeObservationToken_);
    minimizeObservationToken_ = 0;
    pendingMinimizes_.Clear();
    minimizeTargetProvider_ = std::move(target);
    minimizeCompletedCallback_ = std::move(completed);
}

std::optional<DockWindowTransition::MinimizeTarget> DockWindowTransition::ResolveMinimizeTarget(
    HWND source, std::optional<RECT> anchor) const
{
    return minimizeTargetProvider_ ? minimizeTargetProvider_(source, anchor) : std::nullopt;
}

void DockWindowTransition::ObserveMinimize(HWND source, std::optional<RECT> anchor, bool explicitAction)
{
    if (!animationScheduler_ || !minimizeCompletedCallback_ || !source || !IsWindow(source)) return;
    if (const HWND root = GetAncestor(source, GA_ROOT)) source = root;
    if (explicitAction && IsIconic(source) &&
        !(IsActiveFor(source) && direction_ == DockWindowTransitionDirection::Restore)) return;
    DWORD process = 0;
    GetWindowThreadProcessId(source, &process);
    if (!explicitAction)
    {
        if (!pendingMinimizes_.Contains(source, process)) pendingMinimizes_.Forget(source);
        if (pendingMinimizes_.Completed(source) && !IsIconic(source)) pendingMinimizes_.Forget(source);
        if (pendingMinimizes_.Contains(source, process)) return; // Retain the Dock clicked on another screen.
    }
    if (const auto target = ResolveMinimizeTarget(source, anchor))
    {
        pendingMinimizes_.Track(source, process, *target, GetTickCount64());
        ScheduleMinimizeObservation();
    }
}

void DockWindowTransition::ObserveNativeMinimize(HWND source)
{
    ObserveMinimize(source, {}, false);
}

void DockWindowTransition::ForgetNativeMinimize(HWND source)
{
    pendingMinimizes_.Forget(source);
}

void DockWindowTransition::ScheduleMinimizeObservation()
{
    if (!animationScheduler_ || minimizeObservationToken_ || pollingMinimizeObservation_ ||
        !pendingMinimizes_.Waiting()) return;
    minimizeObservationToken_ = animationScheduler_->ScheduleOnce(20, [this](snowdesktop::UiScheduleToken) {
        minimizeObservationToken_ = 0;
        PollMinimizeObservation();
    });
}

void DockWindowTransition::PollMinimizeObservation()
{
    pollingMinimizeObservation_ = true;
    pendingMinimizes_.Poll(GetTickCount64(), [this](HWND source) {
        snowdesktop::dock_minimize_completion::WindowState state;
        if (!IsWindow(source)) return state;
        GetWindowThreadProcessId(source, &state.process);
        state.minimized = IsIconic(source) != FALSE;
        const bool active = IsActiveFor(source);
        state.minimizing = active && GetDirection() == DockWindowTransitionDirection::Minimize;
        state.restoring = active && GetDirection() == DockWindowTransitionDirection::Restore;
        return state;
    }, [this](const MinimizeTarget& target) {
        if (minimizeCompletedCallback_) minimizeCompletedCallback_(target);
    });
    pollingMinimizeObservation_ = false;
    ScheduleMinimizeObservation();
}

bool DockWindowTransition::StartExternalMinimize(HWND sourceWindow, RECT dockRect,
    DWORD deadline, HWND keepBelowWindow)
{
    if (IsActive() || !snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()))
        return false;
    if (!StartMinimize(sourceWindow, dockRect,
            keepBelowWindow))
        return false;
    externalMinimize_ = true;
    externalMinimizeObserved_ = IsIconic(sourceWindow) != FALSE;
    externalMinimizeDeadline_ = deadline;
    externalMinimizeObservationDeadline_ = GetTickCount() +
        snowdesktop::dock_minimize::kRequestTimeoutMs;
    if (!snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()))
    {
        Finish();
        return false;
    }
    return true;
}

void DockWindowTransition::CancelExternalMinimize(HWND sourceWindow, DWORD deadline)
{
    if (externalMinimize_ && sourceWindow_ == sourceWindow && externalMinimizeDeadline_ == deadline)
    {
        if (awaitingMinimizePresentation_)
        {
            if (pendingExternalRestore_) CompleteImmediately();
            else Finish();
            return;
        }
        // This message reports a SendMessageTimeout failure. The hook still
        // executes the original action, so cancellation is not a native veto.
        // Do not extend the observation deadline on repeated timeout reports.
        if (IsIconic(sourceWindow_)) externalMinimizeObserved_ = true;
    }
}

bool DockWindowTransition::StartRestore(
    HWND sourceWindow, RECT dockRect,
    RestoreCallback restoreCallback,
    HWND keepBelowWindow)
{
    if (StartHelperAnimation(sourceWindow, dockRect, keepBelowWindow, true, restoreCallback)) return true;
    if (!restoreCallback)
        return false;
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Restore,
        std::move(restoreCallback),
        keepBelowWindow);
}

bool DockWindowTransition::Start(
    HWND sourceWindow, RECT dockRect,
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback,
    HWND keepBelowWindow)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "prepare");
    nativeFallbackRequested_ = false;
    if (!SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow) ||
        !IsUsableRect(dockRect))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
    std::erase_if(lastVisibleRects_, [](const auto& entry) { return !IsWindow(entry.first); });
    if (IsActiveFor(sourceWindow) && pendingExternalRestore_ &&
        direction == DockWindowTransitionDirection::Minimize)
    {
        pendingExternalRestore_ = false;
        restoreCallback_ = {};
        if (!awaitingMinimizePresentation_ && !ScheduleAnimationWake()) CompleteImmediately();
        return IsActive();
    }
    if (IsActiveFor(sourceWindow) && pendingExternalRestore_ &&
        direction == DockWindowTransitionDirection::Restore)
    {
        restoreCallback_ = std::move(restoreCallback);
        return true;
    }
    const auto startAction =
        ResolveDockWindowTransitionStartAction(
            IsActive(),
            sourceWindow_ == sourceWindow,
            direction_ == direction,
            awaitingRestoreVisibility_);
    if (startAction ==
        DockWindowTransitionStartAction::ContinueActive)
    {
        if (direction ==
                DockWindowTransitionDirection::Restore &&
            restoreCallback)
        {
            restoreCallback_ =
                std::move(restoreCallback);
        }
        return true;
    }
    if (startAction ==
        DockWindowTransitionStartAction::ReverseActive)
    {
        if (awaitingMinimizePresentation_)
        {
            if (externalMinimize_)
            {
                // The CBT hook must still execute its original operation.
                // Keep the aligned image, then reverse under the same cloak
                // only after that native minimize has actually happened.
                pendingExternalRestore_ = true;
                restoreCallback_ = std::move(restoreCallback);
                return true;
            }
            // No native minimize or cloak has occurred yet. Retire the exact
            // aligned duplicate and activate the still-visible real window.
            const bool current = SourceIdentityCurrent();
            Finish();
            if (current && restoreCallback)
                restoreCallback(sourceWindow, DockWindowRestoreTransitionPhase::ActivateRestored);
            return current;
        }
        return Reverse(
            direction,
            std::move(restoreCallback));
    }
    if (startAction ==
        DockWindowTransitionStartAction::
            InterruptRestoreHandoff)
    {
        // The restore image has already reached its destination and the real
        // window is only waiting for the asynchronous SW_RESTORE to settle.
        // A new minimize request must remove that old image immediately;
        // otherwise it can mask the new native request for the entire cleanup
        // timeout. Start a fresh custom transition only when the real window
        // is already available to capture.
        Finish();
        if (direction ==
                DockWindowTransitionDirection::Minimize &&
            IsIconic(sourceWindow))
            return false;
    }

    Cancel();
    if (sourceCloaked_) return false;
    sourceWindow_ = sourceWindow;
    sourceThread_ = GetWindowThreadProcessId(sourceWindow_, &sourceProcess_);
    direction_ = direction;
    restoreCallback_ = std::move(restoreCallback);
    effect_ = snowdesktop::animation::RuntimeWindowEffect();
    const int requestedEffect = effect_;
    imageSource_ = L"unavailable";
    imageResult_ = S_OK;
    genieEdge_ = static_cast<snowdesktop::dock_genie::Edge>(
        std::clamp(snowdesktop::animation::RuntimeDockPosition(), 0, 3));

    RECT windowRect{};
    if (direction_ == DockWindowTransitionDirection::Minimize)
    {
        if (!ResolveVisibleWindowRect(sourceWindow_, windowRect))
        {
            Cancel();
            return false;
        }
        lastVisibleRects_[sourceWindow_] = windowRect;
    }
    else
    {
        // 失败检查：恢复动画只应在窗口确实处于最小化时播放。最小化命令
        // 可能被无响应进程丢弃（窗口从未进入 IsIconic），此时播放动画只会
        // 在幻影位置等待一个永远不会发生的恢复，看起来像动画卡死。
        if (!ResolveRestoreWindowRect(sourceWindow_, windowRect) ||
            !IsIconic(sourceWindow_))
        {
            Cancel();
            return false;
        }
    }
    windowRect_ = windowRect;
    dockRect_ = dockRect;
    fromRect_ = direction_ ==
            DockWindowTransitionDirection::Minimize
        ? windowRect_ : dockRect_;
    toRect_ = direction_ ==
            DockWindowTransitionDirection::Minimize
        ? dockRect_ : windowRect_;
    if (effect_ == 2)
        fromRect_ = toRect_ = windowRect_;
    collapseFrom_ = direction_ == DockWindowTransitionDirection::Minimize ? 0.0 : 1.0;
    collapseTo_ = 1.0 - collapseFrom_;
    lastCollapse_ = collapseFrom_;
    animationFromOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 0.0);
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ = TransitionDuration(effect_);

    if (!EnsureWindow())
    {
        Cancel();
        return false;
    }

    if (!CreateSharedWindowImage())
    {
        LogPresentation(requestedEffect, L"native-system-animation");
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }

    if (!EnumDisplayMonitors(nullptr, nullptr, CollectAnimationMonitor,
            reinterpret_cast<LPARAM>(&animationMonitorRects_)))
    {
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }
    // A fresh restore starts with a transparent point at the Dock. Its full
    // envelope must use actual restored geometry, after native preparation.
    imageHostRect_ =
        effect_ == 4 && direction_ == DockWindowTransitionDirection::Restore ? dockRect_ : ResolveImageHostRect();
    const auto safeHost = ResolveDockWindowNonFullscreenHostRect(
        imageHostRect_, animationMonitorRects_);
    if (!safeHost)
    {
        if (diagnosticCallback_)
            diagnosticCallback_(L"Dock animation fallback: host would cover a complete monitor");
        Cancel();
        nativeFallbackRequested_ = true;
        return false;
    }
    imageHostRect_ = *safeHost;
    const int hostWidth = std::max(
        1L, imageHostRect_.right -
            imageHostRect_.left);
    const int hostHeight = std::max(
        1L, imageHostRect_.bottom -
            imageHostRect_.top);
    // The host callback keeps entire Dock content/backdrop pairs above us.
    // Only legacy callers without that callback use the single-window anchor.
    const HWND insertAfter = !presentationCallback_ &&
        keepBelowWindow && IsWindow(keepBelowWindow)
        ? keepBelowWindow : HWND_TOPMOST;
    SetWindowPos(
        hwnd_, insertAfter,
        imageHostRect_.left,
        imageHostRect_.top,
        hostWidth, hostHeight,
        SWP_NOACTIVATE);

    if (!ApplyFrame(0.0))
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=first-frame");
        Cancel();
        return false;
    }

    presenting_ = true;
    if (presentationCallback_)
        presentationCallback_(hwnd_);
    RefreshOcclusion();
    if (!presenting_)
        return false;
    if (diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-before-show");
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-after-show");
    if (RequiresDockWindowTransitionCompositionBarrier(direction_))
    {
        // Keep the source visible and the overlay at its exact first frame.
        // Neither a compositor timeline nor a native minimize runs until the
        // worker acknowledges this commit. No worker owns an HWND or callback.
        if (!GetWindowRect(sourceWindow_, &minimizeSourceWindowRect_) ||
            !SetPropW(sourceWindow_, snowdesktop::dock_minimize::kPresentationPendingProperty, hwnd_))
        {
            Cancel();
            return false;
        }
        awaitingMinimizePresentation_ = true;
        if (!BeginPresentationBarrier(RestorePresentationPhase::None) || !ScheduleAnimationWake())
        {
            Cancel();
            return false;
        }
        LogPresentation(requestedEffect, L"shared-window");
        return true;
    }

    if (!AcquireSourceCloak()) { Cancel(); return false; }
    animationStartTimeMs_ = MonotonicTimeMilliseconds();
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    awaitingRestoreVisibility_ = false;
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    RequestRestoreForAnimation();
    if (!ScheduleAnimationWake())
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=schedule");
        Cancel();
        return false;
    }
    LogPresentation(requestedEffect, L"shared-window");
    return true;
}

bool DockWindowTransition::SourceIdentityCurrent() const
{
    DWORD process = 0;
    return sourceWindow_ && sourceThread_ &&
        GetWindowThreadProcessId(sourceWindow_, &process) == sourceThread_ && process == sourceProcess_ &&
        (!awaitingMinimizePresentation_ ||
            GetPropW(sourceWindow_, snowdesktop::dock_minimize::kPresentationPendingProperty) == hwnd_);
}

bool DockWindowTransition::AcquireSourceCloak()
{
    // The animation owns only its temporary app-cloak bit. DWM continues to
    // compose this window, so its shared image stays available. Avoid changing
    // the source's setter-only native transition policy, which cannot be read
    // reliably and may already have been disabled by the application.
    bool acquired = false;
    const HRESULT cloakHr = snowdesktop::dock_source_cloak::Acquire(sourceWindow_, hwnd_, acquired);
    if (acquired)
    {
        sourceCloaked_ = true;
        sourceCloakWindow_ = sourceWindow_;
    }
    if (FAILED(cloakHr))
    {
        if (diagnosticCallback_)
        {
            wchar_t diagnostic[128]{};
            swprintf_s(diagnostic, L"Dock animation aborted: stage=source-cloak hr=0x%08X",
                static_cast<unsigned>(cloakHr));
            diagnosticCallback_(diagnostic);
        }
        return false;
    }
    return true;
}

void DockWindowTransition::ClearExternalPresentationWait()
{
    if (SourceIdentityCurrent() &&
        GetPropW(sourceWindow_, snowdesktop::dock_minimize::kPresentationPendingProperty) == hwnd_)
        RemovePropW(sourceWindow_, snowdesktop::dock_minimize::kPresentationPendingProperty);
}

bool DockWindowTransition::PrepareMinimizePresentation(double now)
{
    RECT frame{}, actual{};
    if (!IsWindowVisible(sourceWindow_) || IsIconic(sourceWindow_) ||
        !GetWindowRect(sourceWindow_, &actual) || !EqualRect(&actual, &minimizeSourceWindowRect_) ||
        !ResolveVisibleWindowRect(sourceWindow_, frame) || !EqualRect(&frame, &windowRect_))
    {
        // A source that moved or changed state must not leave an old aligned
        // duplicate visible until a slow compositor wait eventually finishes.
        CompleteImmediately();
        return false;
    }
    const auto result = PresentationBarrierResult(now);
    if (!result) return false;
    presentationBarrier_.reset();
    const bool ready = SUCCEEDED(*result) && SourceIdentityCurrent() &&
        IsWindowVisible(sourceWindow_) && !IsIconic(sourceWindow_) &&
        ResolveVisibleWindowRect(sourceWindow_, frame) && EqualRect(&frame, &windowRect_) &&
        (!externalMinimize_ || snowdesktop::dock_minimize::RequestIsCurrent(externalMinimizeDeadline_, GetTickCount()));
    if (!ready || !AcquireSourceCloak())
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=presentation-barrier");
        CompleteImmediately();
        return false;
    }
    awaitingMinimizePresentation_ = false;
    ClearExternalPresentationWait();
    animationStartTimeMs_ = MonotonicTimeMilliseconds();
    if (externalMinimize_)
        externalMinimizeObservationDeadline_ = GetTickCount() + snowdesktop::dock_minimize::kRequestTimeoutMs;
    minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    if (pendingExternalRestore_) return false; // Poll the original native action before reversal.
    auto request = std::move(minimizeRequest_);
    const HWND source = sourceWindow_;
    if (!ScheduleAnimationWake())
    {
        Finish();
    }
    // The native route is unchanged; it now follows the acknowledged frame and
    // source cloak. Move it first so reentrant cancellation cannot replay it.
    if (request) request(source);
    return true;
}

HRESULT CreateSmoothStepAnimation(
    IDCompositionDesktopDevice* device,
    float from, float to,
    double durationMilliseconds,
    IDCompositionAnimation** animation)
{
    if (!device || !animation || durationMilliseconds <= 0.0)
        return E_INVALIDARG;
    *animation = nullptr;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> result;
    HRESULT hr = device->CreateAnimation(&result);
    const double duration = durationMilliseconds / 1000.0;
    const float delta = to - from;
    if (SUCCEEDED(hr))
    {
        hr = result->AddCubic(
            0.0, from, 0.0f,
            static_cast<float>(3.0 * delta /
                (duration * duration)),
            static_cast<float>(-2.0 * delta /
                (duration * duration * duration)));
    }
    if (SUCCEEDED(hr))
        hr = result->End(duration, to);
    if (SUCCEEDED(hr))
        *animation = result.Detach();
    return hr;
}

bool DockWindowTransition::Reverse(
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback)
{
    if (!IsActive() ||
        !hasLastFrame_ ||
        awaitingRestoreVisibility_)
        return false;

    externalMinimize_ = false;
    externalMinimizeObserved_ = false;
    externalMinimizeDeadline_ = 0;
    externalMinimizeObservationDeadline_ = 0;

    if (!awaitingRestoreVisibility_)
    {
        const double progress = preparingRestore_ ? 0.0
                                                  : std::clamp((MonotonicTimeMilliseconds() - animationStartTimeMs_) /
                                                                   std::max(1.0, animationDurationMs_),
                                                               0.0, 1.0);
        if (!ApplyFrame(progress))
            return false;
        compositionTimelineActive_ = false;
    }

    const RECT currentFrame = lastFrameRect_;
    const RECT targetFrame =
        direction ==
            DockWindowTransitionDirection::Minimize
        ? dockRect_ : windowRect_;
    const auto maximumEdgeDistance =
        [](const RECT& first,
            const RECT& second) {
            return std::max({
                std::abs(
                    static_cast<double>(
                        first.left) -
                    static_cast<double>(
                        second.left)),
                std::abs(
                    static_cast<double>(
                        first.top) -
                    static_cast<double>(
                        second.top)),
                std::abs(
                    static_cast<double>(
                        first.right) -
                    static_cast<double>(
                        second.right)),
                std::abs(
                    static_cast<double>(
                        first.bottom) -
                    static_cast<double>(
                        second.bottom))
            });
        };
    const double fullDistance = std::max(
        1.0,
        maximumEdgeDistance(
            windowRect_, dockRect_));
    double remainingRatio = std::clamp(
        maximumEdgeDistance(
            currentFrame, targetFrame) /
            fullDistance,
        0.0, 1.0);

    const double targetCollapse =
        direction == DockWindowTransitionDirection::Minimize ? 1.0 : 0.0;
    if (effect_ == 3 || effect_ == 4)
        remainingRatio = std::abs(targetCollapse - lastCollapse_);
    else if (effect_ == 2)
        remainingRatio = std::abs(static_cast<double>(lastFrameOpacity_) -
            (direction == DockWindowTransitionDirection::Minimize ? 0.0 : 255.0)) / 255.0;

    direction_ = direction;
    fromRect_ = currentFrame;
    toRect_ = targetFrame;
    if (effect_ == 2)
        fromRect_ = toRect_ = windowRect_;
    collapseFrom_ = lastCollapse_;
    collapseTo_ = targetCollapse;
    animationFromOpacity_ =
        lastFrameOpacity_;
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ = std::clamp(
        TransitionDuration(effect_) * remainingRatio,
        static_cast<double>(
            kMinimumReverseDurationMs) * snowdesktop::animation::RuntimeDurationScale(),
        TransitionDuration(effect_));
    restoreCallback_ =
        std::move(restoreCallback);
    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    awaitingRestoreVisibility_ = false;
    preparingRestore_ = false;
    restoreRequested_ = false;
    restoreActivated_ = false;
    restoreGeometryStableTimeMs_ = 0.0;
    presentationBarrier_.reset();
    restorePresentationPhase_ = RestorePresentationPhase::None;
    restorePresentationAcknowledged_ = false;
    RequestRestoreForAnimation();
    if (!ScheduleAnimationWake())
    {
        CompleteRestoreAfterRenderFailure();
        Finish();
        return false;
    }
    return true;
}

bool DockWindowTransition::ResolveVisibleWindowRect(
    HWND window, RECT& rect) const
{
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &rect, sizeof(rect))) &&
        IsUsableRect(rect))
        return true;
    return GetWindowRect(window, &rect) &&
        IsUsableRect(rect);
}

bool DockWindowTransition::ResolveRestoreWindowRect(
    HWND window, RECT& rect) const
{
    const auto cached = lastVisibleRects_.find(window);
    if (cached != lastVisibleRects_.end() &&
        IsUsableRect(cached->second))
    {
        rect = cached->second;
        return true;
    }

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement))
        return false;

    if (snowdesktop::dock_window_rules::
            ShouldRestoreDockWindowMaximized(
                placement.flags, placement.showCmd))
    {
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        const HMONITOR monitor = MonitorFromWindow(
            window, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfoW(monitor, &monitorInfo))
        {
            rect = monitorInfo.rcWork;
            return IsUsableRect(rect);
        }
    }

    rect = placement.rcNormalPosition;
    return IsUsableRect(rect);
}

bool DockWindowTransition::EnsureCompositionVisuals()
{
    if (!compositionDevice_)
    {
        imageResult_ = E_NOTIMPL;
        return false;
    }
    if (!ReserveSceneRetirement() || !EnsureWindow())
    {
        imageResult_ = E_OUTOFMEMORY;
        return false;
    }
    if (!compositionDevice_ || !hwnd_ || !IsWindow(hwnd_))
    {
        imageResult_ = E_NOTIMPL;
        return false;
    }
    HRESULT hr = S_OK;
    if (!compositionTarget_)
    {
        hr = compositionDevice_->CreateTargetForHwnd(
            hwnd_, TRUE, &compositionTarget_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateVisual(
                &compositionVisual_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateEffectGroup(
                &compositionEffect_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateScaleTransform(
                &compositionScaleTransform_);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateRectangleClip(
                &compositionClip_);
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetEffect(
                compositionEffect_.Get());
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetClip(
                compositionClip_.Get());
        if (SUCCEEDED(hr))
            hr = compositionVisual_->SetTransform(
                compositionScaleTransform_.Get());
        if (SUCCEEDED(hr))
            hr = compositionTarget_->SetRoot(
                compositionVisual_.Get());
        if (FAILED(hr))
        {
            imageResult_ = hr;
            compositionClip_.Reset();
            compositionEffect_.Reset();
            compositionScaleTransform_.Reset();
            compositionVisual_.Reset();
            compositionTarget_.Reset();
            return false;
        }
    }

    return true;
}

bool DockWindowTransition::CreateSharedWindowImage()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "image.bind");
    compositionSharedWindowActive_ = false;
    SIZE size{};
    imageResult_ = snowdesktop::dock_thumbnail::SourceSize(sourceWindow_, size);
    if (FAILED(imageResult_) || !EnsureCompositionVisuals()) return false;
    compositionImageSize_ = size;
    compositionSourceRegion_ = snowdesktop::dock_thumbnail::SourceRegion(sourceWindow_, size);
    compositionSharedWindowActive_ = true;
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = compositionEffect_->SetOpacity(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetLeft(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetTop(0.0f);
    if (SUCCEEDED(hr)) hr = compositionClip_->SetRight(static_cast<float>(size.cx));
    if (SUCCEEDED(hr)) hr = compositionClip_->SetBottom(static_cast<float>(size.cy));
    if (SUCCEEDED(hr) && (effect_ == 3 || effect_ == 4))
    {
        if (!CreateGenieStrips()) hr = FAILED(imageResult_) ? imageResult_ : E_FAIL;
    }
    else if (SUCCEEDED(hr))
    {
        hr = compositionWindowImage_.Create(hwnd_, sourceWindow_, compositionDevice_.Get(), size,
            &compositionSourceRegion_);
        if (SUCCEEDED(hr)) hr = compositionVisual_->AddVisual(compositionWindowImage_.visual.Get(), TRUE, nullptr);
    }
    if (FAILED(hr))
    {
        compositionVisual_->RemoveAllVisuals();
        ClearGenieStrips();
        compositionWindowImage_.Reset();
        compositionSharedWindowActive_ = false;
        compositionImageSize_ = {};
        imageResult_ = hr;
        return false;
    }
    imageSource_ = L"dwm-shared-window";
    imageResult_ = S_OK;
    compositionImageActive_ = true;
    return true;
}

void DockWindowTransition::ClearGenieStrips()
{
    if (genieStrips_.empty() && attractionTriangles_.empty())
        return;
    if (compositionVisual_)
        compositionVisual_->RemoveAllVisuals();
    for (auto& strip : genieStrips_)
        strip->SetContent(nullptr);
    genieStrips_.clear();
    for (auto &triangle : attractionTriangles_)
        triangle->RemoveAllVisuals();
    attractionTriangles_.clear();
    genieWindowImages_.clear();
    if (compositionVisual_)
    {
        compositionVisual_->SetContent(nullptr);
        compositionVisual_->SetTransform(compositionScaleTransform_.Get());
        compositionVisual_->SetClip(compositionClip_.Get());
    }
    hasLastFrame_ = false;
}

bool DockWindowTransition::CreateGenieStrips()
{
    if (effect_ == 4)
        return CreateAttractionMesh();
    if (!compositionVisual_ || !compositionSharedWindowActive_ || !compositionDevice_)
    {
        imageResult_ = E_UNEXPECTED;
        return false;
    }
    const std::size_t count = snowdesktop::dock_genie::StripCount;
    genieStrips_.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto band = ResolveGenieSourceBand(compositionImageSize_, genieEdge_, i, effect_ == 4);
        const D2D1_RECT_F clip = band.clip;
        // Each DWM relationship supplies only this source band, rather than
        // another full-size window image. Keep integer pixel crops unscaled
        // and preserve the original fractional overlap inside the crop.
        const RECT crop = band.crop;
        const SIZE size{crop.right - crop.left, crop.bottom - crop.top};
        const RECT source{compositionSourceRegion_.left + crop.left, compositionSourceRegion_.top + crop.top,
            compositionSourceRegion_.left + crop.right, compositionSourceRegion_.top + crop.bottom};
        snowdesktop::dock_thumbnail::SharedVisual image;
        HRESULT hr = image.Create(hwnd_, sourceWindow_, compositionDevice_.Get(), size, &source);
        if (SUCCEEDED(hr) && !image.visual)
            hr = E_UNEXPECTED;
        Microsoft::WRL::ComPtr<IDCompositionVisual3> strip = image.visual;
        if (FAILED(hr) || !strip)
        {
            imageResult_ = FAILED(hr) ? hr : E_FAIL;
            return false;
        }
        genieWindowImages_.push_back(std::move(image));
        genieStrips_.push_back(strip);
        // Keep clips in cropped-image coordinates. The mesh and timeline
        // explicitly translate that origin inside their projective matrices.
        const float cropLeft = static_cast<float>(crop.left), cropTop = static_cast<float>(crop.top);
        if (SUCCEEDED(hr)) hr = strip->SetClip(D2D1::RectF(
            clip.left - cropLeft, clip.top - cropTop, clip.right - cropLeft, clip.bottom - cropTop));
        if (SUCCEEDED(hr)) hr = strip->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
        if (SUCCEEDED(hr)) hr = strip->SetBitmapInterpolationMode(
            DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (SUCCEEDED(hr))
            hr = compositionVisual_->AddVisual(strip.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            imageResult_ = hr;
            return false;
        }
    }
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetClip(static_cast<IDCompositionClip *>(nullptr));
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetX(0.0f);
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetY(0.0f);
    imageResult_ = hr;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::CreateAttractionMesh()
{
    namespace attraction = snowdesktop::dock_attraction;
    if (!compositionVisual_ || !compositionSharedWindowActive_ || !compositionDevice_)
    {
        imageResult_ = E_UNEXPECTED;
        return false;
    }
    attractionTriangles_.reserve(attraction::TriangleCount);
    genieWindowImages_.reserve(attraction::TriangleCount);
    for (std::size_t triangle = 0; triangle < attraction::TriangleCount; ++triangle)
    {
        const auto cell = attraction::SourceCell(triangle / 2);
        const auto band = ResolveGenieSourceBand(compositionImageSize_, genieEdge_, triangle / 2, true);
        const RECT crop = band.crop;
        const SIZE size{crop.right - crop.left, crop.bottom - crop.top};
        const RECT source{compositionSourceRegion_.left + crop.left, compositionSourceRegion_.top + crop.top,
                          compositionSourceRegion_.left + crop.right, compositionSourceRegion_.top + crop.bottom};
        snowdesktop::dock_thumbnail::SharedVisual image;
        HRESULT hr = image.Create(hwnd_, sourceWindow_, compositionDevice_.Get(), size, &source);
        Microsoft::WRL::ComPtr<IDCompositionVisual2> clipper;
        if (SUCCEEDED(hr) && !image.visual)
            hr = E_UNEXPECTED;
        if (SUCCEEDED(hr))
            hr = compositionDevice_->CreateVisual(&clipper);
        const float width = static_cast<float>((cell.right - cell.left) * compositionImageSize_.cx);
        const float height = static_cast<float>((cell.bottom - cell.top) * compositionImageSize_.cy);
        const float left = static_cast<float>(cell.left * compositionImageSize_.cx - crop.left);
        const float top = static_cast<float>(cell.top * compositionImageSize_.cy - crop.top);
        const float sign = triangle % 2 == 0 ? 1.0f : -1.0f;
        const float offsetS = triangle % 2 == 0 ? 0.0f : 2.0f;
        const float offsetT = triangle % 2 == 0 ? 0.0f : 1.0f;
        const D2D1_MATRIX_3X2_F toTriangle{sign / width,
                                           0,
                                           sign / height,
                                           sign / height,
                                           offsetS - sign * (left / width + top / height),
                                           offsetT - sign * top / height};
        if (SUCCEEDED(hr))
            hr = static_cast<IDCompositionVisual *>(image.visual.Get())->SetTransform(toTriangle);
        const float cropLeft = static_cast<float>(crop.left), cropTop = static_cast<float>(crop.top);
        if (SUCCEEDED(hr))
            hr = image.visual->SetClip(D2D1::RectF(band.clip.left - cropLeft, band.clip.top - cropTop,
                                                   band.clip.right - cropLeft, band.clip.bottom - cropTop));
        if (SUCCEEDED(hr))
            hr = image.visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        if (SUCCEEDED(hr))
            hr = image.visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        // The source rectangle becomes a parallelogram in (s,t). Clipping
        // s<=1 selects an affine triangle without any perspective division.
        // Tiny overlap covers float roundoff; internal clips must not add AA
        // transparency independently on either side of a shared edge.
        const float diagonalGuard = std::min(0.02f, 0.25f / std::max(width, height));
        if (SUCCEEDED(hr))
            hr = clipper->SetClip(D2D1::RectF(-16, -16, 1 + diagonalGuard, 16));
        if (SUCCEEDED(hr))
            hr = clipper->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
        if (SUCCEEDED(hr))
            hr = clipper->SetBackFaceVisibility(DCOMPOSITION_BACKFACE_VISIBILITY_VISIBLE);
        if (SUCCEEDED(hr))
            hr = clipper->AddVisual(image.visual.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr))
            hr = compositionVisual_->AddVisual(clipper.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            imageResult_ = hr;
            return false;
        }
        genieWindowImages_.push_back(std::move(image));
        attractionTriangles_.push_back(std::move(clipper));
    }
    HRESULT hr = compositionVisual_->SetContent(nullptr);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetClip(static_cast<IDCompositionClip*>(nullptr));
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetX(0.0f);
    if (SUCCEEDED(hr)) hr = compositionVisual_->SetOffsetY(0.0f);
    imageResult_ = hr;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::ApplyGenieFrame(double collapsed, BYTE opacity)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "genie.submit");
    if (genieStrips_.size() != snowdesktop::dock_genie::StripCount ||
        !compositionDevice_ || !compositionEffect_)
        return false;
    const auto rect = [](const RECT& value) {
        return snowdesktop::dock_genie::Rect{
            static_cast<double>(value.left), static_cast<double>(value.top),
            static_cast<double>(value.right), static_cast<double>(value.bottom)};
    };
    HRESULT hr = S_OK;
    const bool geometryChanged = !hasLastFrame_ || lastGenieCollapse_ != collapsed;
    const bool opacityChanged = !hasLastFrame_ || lastFrameOpacity_ != opacity;
    if (!geometryChanged && !opacityChanged) return true;
    for (std::size_t i = 0; geometryChanged && i < genieStrips_.size() && SUCCEEDED(hr); ++i)
    {
        const RECT crop = ResolveGenieSourceBand(compositionImageSize_, genieEdge_, i).crop;
        const auto transform = snowdesktop::dock_genie::SourceLocalMatrix(
            snowdesktop::dock_genie::StripProjectiveMatrix(
            rect(windowRect_), rect(dockRect_), genieEdge_, collapsed,
            compositionImageSize_.cx, compositionImageSize_.cy,
            static_cast<double>(i) / static_cast<double>(genieStrips_.size()),
            static_cast<double>(i + 1) / static_cast<double>(genieStrips_.size()),
            imageHostRect_.left, imageHostRect_.top), crop.left, crop.top);
        const D2D1_MATRIX_4X4_F matrix{
            static_cast<float>(transform.m11), static_cast<float>(transform.m12), 0, static_cast<float>(transform.m14),
            static_cast<float>(transform.m21), static_cast<float>(transform.m22), 0, static_cast<float>(transform.m24),
            0, 0, 1, 0,
            static_cast<float>(transform.dx), static_cast<float>(transform.dy), 0, static_cast<float>(transform.w)};
        hr = genieStrips_[i]->SetTransform(matrix);
    }
    if (SUCCEEDED(hr) && opacityChanged)
        hr = compositionEffect_->SetOpacity(static_cast<float>(opacity) / 255.0f);
    if (SUCCEEDED(hr)) hr = compositionDevice_->Commit();
    if (SUCCEEDED(hr)) lastGenieCollapse_ = collapsed;
    return SUCCEEDED(hr);
}

bool DockWindowTransition::StartGenieTimeline()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "genie.timeline.prepare");
    namespace genie = snowdesktop::dock_genie;
    if (genieStrips_.size() != genie::StripCount) return false;
    const genie::Rect window{static_cast<double>(windowRect_.left), static_cast<double>(windowRect_.top),
        static_cast<double>(windowRect_.right), static_cast<double>(windowRect_.bottom)};
    const genie::Rect dock{static_cast<double>(dockRect_.left), static_cast<double>(dockRect_.top),
        static_cast<double>(dockRect_.right), static_cast<double>(dockRect_.bottom)};
    const bool vertical = genie::Vertical(genieEdge_);
    const std::array<std::array<int, 2>, 7> elements{{{0, 0},
        vertical ? std::array<int, 2>{1, 0} : std::array<int, 2>{0, 1}, {1, 1},
        vertical ? std::array<int, 2>{1, 3} : std::array<int, 2>{0, 3}, {3, 0}, {3, 1}, {3, 3}}};
    std::vector<Microsoft::WRL::ComPtr<IDCompositionAnimation>> ownedAnimations;
    std::vector<IDCompositionAnimation*> animations;
    ownedAnimations.reserve(genieStrips_.size() * elements.size() + 1);
    animations.reserve(ownedAnimations.capacity());
    const double duration = animationDurationMs_ / 1000.0;
    const double stepSeconds = duration / genie::TimelineSteps;
    HRESULT hr = S_OK;
    for (std::size_t strip = 0; strip < genieStrips_.size() && SUCCEEDED(hr); ++strip)
    {
        Microsoft::WRL::ComPtr<IDCompositionMatrixTransform3D> transform;
        hr = compositionDevice_->CreateMatrixTransform3D(&transform);
        const double begin = static_cast<double>(strip) / static_cast<double>(genieStrips_.size());
        const double end = static_cast<double>(strip + 1) / static_cast<double>(genieStrips_.size());
        const RECT crop = ResolveGenieSourceBand(compositionImageSize_, genieEdge_, strip).crop;
        std::array<genie::TimelineSample, genie::TimelineSteps + 1> samples{};
        for (std::size_t step = 0; step < samples.size(); ++step)
            samples[step] = genie::SampleTimeline(window, dock, genieEdge_, collapseFrom_, collapseTo_,
                static_cast<double>(step) / genie::TimelineSteps, compositionImageSize_.cx,
                compositionImageSize_.cy, begin, end, imageHostRect_.left, imageHostRect_.top,
                crop.left, crop.top);
        for (std::size_t element = 0; element < elements.size() + (strip == 0 ? 1 : 0) && SUCCEEDED(hr); ++element)
        {
            Microsoft::WRL::ComPtr<IDCompositionAnimation> animation;
            hr = compositionDevice_->CreateAnimation(&animation);
            for (std::size_t step = 0; step < genie::TimelineSteps && SUCCEEDED(hr); ++step)
            {
                const auto curve = genie::TimelineSegment(samples[step], samples[step + 1], element, duration);
                hr = animation->AddCubic(step * stepSeconds,
                    curve.constant, curve.linear, curve.quadratic, curve.cubic);
            }
            if (SUCCEEDED(hr)) hr = animation->End(duration, static_cast<float>(samples.back().value[element]));
            if (SUCCEEDED(hr))
            {
                if (element == elements.size()) hr = compositionEffect_->SetOpacity(animation.Get());
                else hr = transform->SetMatrixElement(
                    elements[element][0], elements[element][1], animation.Get());
            }
            if (SUCCEEDED(hr))
            {
                animations.push_back(animation.Get());
                ownedAnimations.push_back(std::move(animation));
            }
        }
        if (SUCCEEDED(hr)) hr = genieStrips_[strip]->SetTransform(transform.Get());
    }
    return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
}

RECT DockWindowTransition::ResolveImageHostRect() const
{
    RECT host{};
    if (effect_ == 4)
    {
        const auto bounds =
            snowdesktop::dock_attraction::Bounds(DeformationRect(windowRect_), DeformationRect(dockRect_));
        host = {static_cast<LONG>(bounds.left), static_cast<LONG>(bounds.top), static_cast<LONG>(bounds.right),
                static_cast<LONG>(bounds.bottom)};
    }
    else if (effect_ == 3)
        host = ResolveDockWindowImageHostRect(windowRect_, dockRect_);
    else
        return UsesCompositionImage() ? ResolveDockWindowImageHostRect(fromRect_, toRect_) : fromRect_;

    const bool coversMonitor =
        std::any_of(animationMonitorRects_.begin(), animationMonitorRects_.end(), [&](RECT monitor) {
            return host.left <= monitor.left && host.top <= monitor.top && host.right >= monitor.right &&
                   host.bottom >= monitor.bottom;
        });
    if (coversMonitor)
    {
        RECT desktop = animationMonitorRects_.front();
        for (const RECT monitor : animationMonitorRects_)
            UnionRect(&desktop, &desktop, &monitor);
        RECT visible{};
        // A restored maximized HWND can include resize borders outside the
        // desktop. Keep the full cross-monitor deformation, but discard only
        // pixels no display can show before applying the one-pixel Shell guard.
        if (IntersectRect(&visible, &host, &desktop))
            host = visible;
    }
    return host;
}

bool DockWindowTransition::ApplyAttractionFrame(double collapsed, BYTE opacity)
{
    namespace attraction = snowdesktop::dock_attraction;
    if (attractionTriangles_.size() != attraction::TriangleCount)
        return false;
    const auto frame = attraction::MeshFrame(DeformationRect(windowRect_), DeformationRect(dockRect_), collapsed);
    HRESULT hr = S_OK;
    for (std::size_t triangle = 0; triangle < attractionTriangles_.size() && SUCCEEDED(hr); ++triangle)
    {
        const auto value = attraction::TriangleValues(frame, triangle, imageHostRect_.left, imageHostRect_.top);
        const D2D1_MATRIX_3X2_F matrix{static_cast<float>(value[0]), static_cast<float>(value[1]),
                                       static_cast<float>(value[2]), static_cast<float>(value[3]),
                                       static_cast<float>(value[4]), static_cast<float>(value[5])};
        hr = attractionTriangles_[triangle]->SetTransform(matrix);
    }
    if (SUCCEEDED(hr))
        hr = compositionEffect_->SetOpacity(static_cast<float>(opacity) / 255.0f);
    if (SUCCEEDED(hr))
        hr = compositionDevice_->Commit();
    return SUCCEEDED(hr);
}

bool DockWindowTransition::StartAttractionTimeline()
{
    namespace attraction = snowdesktop::dock_attraction;
    if (attractionTriangles_.size() != attraction::TriangleCount)
        return false;
    const auto window = DeformationRect(windowRect_), dock = DeformationRect(dockRect_);
    std::array<attraction::Frame, attraction::TimelineSteps + 1> frames{};
    for (std::size_t step = 0; step < frames.size(); ++step)
        frames[step] = attraction::TimelineFrame(window, dock, collapseFrom_, collapseTo_,
                                                 static_cast<double>(step) / attraction::TimelineSteps);
    const auto tangents = attraction::MeshTangents(frames);
    constexpr std::array<std::array<int, 2>, 6> elements{{{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}, {2, 1}}};
    std::vector<Microsoft::WRL::ComPtr<IDCompositionAnimation>> owned;
    std::vector<IDCompositionAnimation *> animations;
    owned.reserve(attractionTriangles_.size() * 6 + 1);
    animations.reserve(owned.capacity());
    const double duration = animationDurationMs_ / 1000.0, stepSeconds = duration / attraction::TimelineSteps;
    HRESULT hr = S_OK;
    for (std::size_t triangle = 0; triangle < attractionTriangles_.size() && SUCCEEDED(hr); ++triangle)
    {
        Microsoft::WRL::ComPtr<IDCompositionMatrixTransform> transform;
        hr = compositionDevice_->CreateMatrixTransform(&transform);
        std::array<attraction::Values, attraction::TimelineSteps + 1> values{}, slopes{};
        for (std::size_t step = 0; step < frames.size(); ++step)
        {
            values[step] = attraction::TriangleValues(frames[step], triangle, imageHostRect_.left, imageHostRect_.top);
            slopes[step] = attraction::TriangleValues(tangents[step], triangle, 0, 0);
        }
        for (std::size_t element = 0; element < (triangle == 0 ? 7u : 6u) && SUCCEEDED(hr); ++element)
        {
            Microsoft::WRL::ComPtr<IDCompositionAnimation> animation;
            hr = compositionDevice_->CreateAnimation(&animation);
            for (std::size_t step = 0; step < attraction::TimelineSteps && SUCCEEDED(hr); ++step)
            {
                const auto curve = attraction::TimelineSegment(values[step], values[step + 1], slopes[step],
                                                               slopes[step + 1], element, duration);
                hr =
                    animation->AddCubic(step * stepSeconds, curve.constant, curve.linear, curve.quadratic, curve.cubic);
            }
            if (SUCCEEDED(hr))
                hr = animation->End(duration, static_cast<float>(values.back()[element]));
            if (SUCCEEDED(hr))
            {
                if (element == 6)
                    hr = compositionEffect_->SetOpacity(animation.Get());
                else
                    hr = transform->SetMatrixElement(elements[element][0], elements[element][1], animation.Get());
            }
            if (SUCCEEDED(hr))
            {
                animations.push_back(animation.Get());
                owned.push_back(std::move(animation));
            }
        }
        if (SUCCEEDED(hr))
            hr = attractionTriangles_[triangle]->SetTransform(transform.Get());
    }
    return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
}

bool DockWindowTransition::CommitCompositionTimeline(
    std::span<IDCompositionAnimation* const> animations)
{
    LARGE_INTEGER begin{}, frequency{};
    if (!QueryPerformanceCounter(&begin) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) return false;
    DCOMPOSITION_FRAME_STATISTICS statistics{};
    if (SUCCEEDED(compositionDevice_->GetFrameStatistics(&statistics)) &&
        statistics.nextEstimatedFrameTime.QuadPart > begin.QuadPart &&
        statistics.nextEstimatedFrameTime.QuadPart - begin.QuadPart < frequency.QuadPart / 10)
        begin = statistics.nextEstimatedFrameTime;
    // All properties and the completion wake use the same clock. Building or
    // rebinding the scene must not consume the beginning of the animation.
    for (auto* animation : animations)
        if (FAILED(animation->SetAbsoluteBeginTime(begin))) return false;
    if (FAILED(compositionDevice_->Commit())) return false;
    animationStartTimeMs_ = static_cast<double>(begin.QuadPart) * 1000.0 /
        static_cast<double>(frequency.QuadPart);
    if (awaitingRestoreVisibility_ && restoreFadeStartTimeMs_ > 0.0)
        restoreFadeStartTimeMs_ = animationStartTimeMs_;
    if (direction_ == DockWindowTransitionDirection::Minimize)
        minimizeCleanupDeadlineMs_ = animationStartTimeMs_ + animationDurationMs_ + kMinimizeCleanupTimeoutMs;
    compositionTimelineActive_ = true;
    return true;
}

bool DockWindowTransition::StartCompositionTimeline(bool opacityOnly)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "timeline.prepare");
    if (!compositionImageActive_ || !compositionDevice_ ||
        !compositionVisual_ || !compositionScaleTransform_ ||
        !compositionEffect_ || !compositionClip_ ||
        compositionImageSize_.cx <= 0 ||
        compositionImageSize_.cy <= 0 ||
        animationDurationMs_ <= 0.0)
        return false;

    if (opacityOnly)
    {
        Microsoft::WRL::ComPtr<IDCompositionAnimation> opacity;
        HRESULT hr = CreateSmoothStepAnimation(compositionDevice_.Get(),
            static_cast<float>(animationFromOpacity_) / 255.0f,
            static_cast<float>(animationToOpacity_) / 255.0f,
            animationDurationMs_, &opacity);
        if (SUCCEEDED(hr)) hr = compositionEffect_->SetOpacity(opacity.Get());
        const std::array<IDCompositionAnimation*, 1> animations{opacity.Get()};
        return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
    }

    if (effect_ == 3)
        return StartGenieTimeline();
    if (effect_ == 4)
        return StartAttractionTimeline();

    const auto width = [](const RECT& rect) {
        return std::max(1L, rect.right - rect.left);
    };
    const auto height = [](const RECT& rect) {
        return std::max(1L, rect.bottom - rect.top);
    };
    const float fromScaleX = static_cast<float>(width(fromRect_)) /
        static_cast<float>(compositionImageSize_.cx);
    const float fromScaleY = static_cast<float>(height(fromRect_)) /
        static_cast<float>(compositionImageSize_.cy);
    const float toScaleX = static_cast<float>(width(toRect_)) /
        static_cast<float>(compositionImageSize_.cx);
    const float toScaleY = static_cast<float>(height(toRect_)) /
        static_cast<float>(compositionImageSize_.cy);
    const float fromRadiusX = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            fromRect_, dockRect_)) / fromScaleX;
    const float fromRadiusY = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            fromRect_, dockRect_)) / fromScaleY;
    const float toRadiusX = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            toRect_, dockRect_)) / toScaleX;
    const float toRadiusY = static_cast<float>(
        ResolveDockWindowTransitionCornerRadius(
            toRect_, dockRect_)) / toScaleY;

    Microsoft::WRL::ComPtr<IDCompositionAnimation> offsetX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> offsetY;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> scaleY;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> opacity;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> radiusX;
    Microsoft::WRL::ComPtr<IDCompositionAnimation> radiusY;
    auto create = [&](float from, float to,
                      IDCompositionAnimation** animation) {
        return CreateSmoothStepAnimation(
            compositionDevice_.Get(), from, to,
            animationDurationMs_, animation);
    };
    HRESULT hr = create(
        static_cast<float>(fromRect_.left - imageHostRect_.left),
        static_cast<float>(toRect_.left - imageHostRect_.left),
        &offsetX);
    if (SUCCEEDED(hr))
        hr = create(
            static_cast<float>(fromRect_.top - imageHostRect_.top),
            static_cast<float>(toRect_.top - imageHostRect_.top),
            &offsetY);
    if (SUCCEEDED(hr))
        hr = create(fromScaleX, toScaleX, &scaleX);
    if (SUCCEEDED(hr))
        hr = create(fromScaleY, toScaleY, &scaleY);
    if (SUCCEEDED(hr))
        hr = create(
            static_cast<float>(animationFromOpacity_) / 255.0f,
            static_cast<float>(animationToOpacity_) / 255.0f,
            &opacity);
    if (SUCCEEDED(hr))
        hr = create(fromRadiusX, toRadiusX, &radiusX);
    if (SUCCEEDED(hr))
        hr = create(fromRadiusY, toRadiusY, &radiusY);
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetX(offsetX.Get());
    if (SUCCEEDED(hr))
        hr = compositionVisual_->SetOffsetY(offsetY.Get());
    if (SUCCEEDED(hr))
        hr = compositionScaleTransform_->SetScaleX(scaleX.Get());
    if (SUCCEEDED(hr))
        hr = compositionScaleTransform_->SetScaleY(scaleY.Get());
    if (SUCCEEDED(hr))
        hr = compositionEffect_->SetOpacity(opacity.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopLeftRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopLeftRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopRightRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetTopRightRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomLeftRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomLeftRadiusY(radiusY.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomRightRadiusX(radiusX.Get());
    if (SUCCEEDED(hr))
        hr = compositionClip_->SetBottomRightRadiusY(radiusY.Get());
    const std::array<IDCompositionAnimation*, 7> animations{
        offsetX.Get(), offsetY.Get(), scaleX.Get(), scaleY.Get(),
        opacity.Get(), radiusX.Get(), radiusY.Get()};
    return SUCCEEDED(hr) && CommitCompositionTimeline(animations);
}

bool DockWindowTransition::ScheduleAnimationWake()
{
    if (!animationScheduler_)
        return false;
    if (animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;

    const bool retiring = awaitingRestoreVisibility_ && restoreFadeStartTimeMs_ > 0.0;
    if (UsesCompositionImage() && !preparingRestore_ && !awaitingMinimizePresentation_)
    {
        if (!StartCompositionTimeline(retiring))
            return false;
        animationToken_ = animationScheduler_->ScheduleOnce(
            static_cast<UINT>(std::ceil(std::max(1.0,
                animationStartTimeMs_ + animationDurationMs_ - MonotonicTimeMilliseconds()))) + 2,
            [this](snowdesktop::UiScheduleToken token) {
                if (animationToken_ != token)
                    return;
                animationToken_ = 0;
                compositionTimelineActive_ = false;
                const bool keep = OnAnimationFrame(
                    MonotonicTimeMilliseconds());
                if (keep && !animationToken_ && animationScheduler_)
                {
                    animationToken_ =
                        animationScheduler_->StartAnimation(
                            snowdesktop::UiAnimationSurface::
                                WindowTransition,
                            [this](double nowMilliseconds) {
                                return OnAnimationFrame(
                                    nowMilliseconds);
                            });
                }
            });
    }
    else
    {
        animationToken_ = animationScheduler_->StartAnimation(
            snowdesktop::UiAnimationSurface::WindowTransition,
            [this](double nowMilliseconds) {
                return OnAnimationFrame(nowMilliseconds);
            });
    }
    return animationToken_ != 0;
}

bool DockWindowTransition::ApplyFrame(double progress)
{
    if (!hwnd_ || !compositionSharedWindowActive_)
        return false;

    RECT frame = InterpolateDockWindowTransitionRect(
        fromRect_, toRect_, progress);
    const int width = std::max(1L, frame.right - frame.left);
    const int height =
        std::max(1L, frame.bottom - frame.top);
    if (UsesCompositionImage() &&
        !ApplyOcclusion(imageHostRect_, 0))
        return false;
    const double eased =
        EaseDockWindowTransition(progress);
    BYTE frameOpacity =
        static_cast<BYTE>(std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(
                    animationFromOpacity_) +
                (static_cast<double>(
                    animationToOpacity_) -
                    static_cast<double>(
                        animationFromOpacity_)) *
                    eased)),
            0, 255));
    if (!awaitingRestoreVisibility_)
        lastCollapse_ = snowdesktop::dock_genie::Mix(collapseFrom_, collapseTo_, eased);
    if (effect_ == 3 || effect_ == 4)
    {
        if (!awaitingRestoreVisibility_)
            frameOpacity = static_cast<BYTE>(std::clamp(
                static_cast<int>(std::lround((effect_ == 4 ? snowdesktop::dock_attraction::Opacity(lastCollapse_)
                                                           : snowdesktop::dock_genie::Opacity(lastCollapse_)) *
                                             255.0)),
                0, 255));
        if (!(effect_ == 4 ? ApplyAttractionFrame(lastCollapse_, frameOpacity)
                           : ApplyGenieFrame(lastCollapse_, frameOpacity)))
            return false;
        lastFrameRect_ = frame;
        lastFrameOpacity_ = frameOpacity;
        hasLastFrame_ = true;
        return true;
    }
    const int cornerRadius =
        ResolveDockWindowTransitionCornerRadius(
            frame, dockRect_);
    const bool geometryChanged =
        !hasLastFrame_ ||
        EqualRect(&frame, &lastFrameRect_) == FALSE;
    const bool opacityChanged =
        !hasLastFrame_ ||
        frameOpacity != lastFrameOpacity_;
    if (!geometryChanged && !opacityChanged)
        return true;

    if (UsesCompositionImage())
    {
        if (!compositionImageActive_ ||
            !compositionDevice_ ||
            !compositionVisual_ ||
            !compositionScaleTransform_ ||
            !compositionEffect_ ||
            !compositionClip_ ||
            compositionImageSize_.cx <= 0 ||
            compositionImageSize_.cy <= 0)
            return false;

        const float scaleX =
            static_cast<float>(width) /
            static_cast<float>(compositionImageSize_.cx);
        const float scaleY =
            static_cast<float>(height) /
            static_cast<float>(compositionImageSize_.cy);
        HRESULT hr = S_OK;
        if (geometryChanged)
        {
            hr = compositionVisual_->SetOffsetX(
                static_cast<float>(
                    frame.left - imageHostRect_.left));
            if (SUCCEEDED(hr))
                hr = compositionVisual_->SetOffsetY(
                    static_cast<float>(
                        frame.top - imageHostRect_.top));
            if (SUCCEEDED(hr))
                hr = compositionScaleTransform_->SetScaleX(scaleX);
            if (SUCCEEDED(hr))
                hr = compositionScaleTransform_->SetScaleY(scaleY);
            const float radiusX = scaleX > 0.0f
                ? static_cast<float>(cornerRadius) / scaleX
                : 0.0f;
            const float radiusY = scaleY > 0.0f
                ? static_cast<float>(cornerRadius) / scaleY
                : 0.0f;
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopLeftRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopLeftRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopRightRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetTopRightRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomLeftRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomLeftRadiusY(radiusY);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomRightRadiusX(radiusX);
            if (SUCCEEDED(hr))
                hr = compositionClip_->SetBottomRightRadiusY(radiusY);
        }
        if (SUCCEEDED(hr) && opacityChanged)
            hr = compositionEffect_->SetOpacity(
                static_cast<float>(frameOpacity) / 255.0f);
        if (SUCCEEDED(hr))
            hr = compositionDevice_->Commit();
        if (FAILED(hr))
            return false;
    }
    lastFrameRect_ = frame;
    lastFrameOpacity_ = frameOpacity;
    hasLastFrame_ = true;
    return true;
}

bool DockWindowTransition::OnAnimationFrame(
    double nowMilliseconds)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "frame");
    if (!SourceIdentityCurrent())
    {
        Finish();
        return false;
    }
    if (externalMinimize_)
    {
        if (awaitingMinimizePresentation_ &&
            !snowdesktop::dock_minimize::RequestIsCurrent(externalMinimizeDeadline_, GetTickCount()))
        {
            if (pendingExternalRestore_) CompleteImmediately();
            else Finish();
            return false;
        }
        if (IsIconic(sourceWindow_)) externalMinimizeObserved_ = true;
        else if (externalMinimizeObserved_ ||
            !snowdesktop::dock_minimize::RequestIsCurrent(externalMinimizeObservationDeadline_, GetTickCount()))
        {
            // Another hook can veto the original action; a restore can also
            // arrive before our image reaches the Dock. Do not cover it.
            if (pendingExternalRestore_) CompleteImmediately();
            else Finish();
            return false;
        }
    }
    if (direction_ == DockWindowTransitionDirection::Minimize)
    {
        if (IsIconic(sourceWindow_)) minimizeObserved_ = true;
        else if (minimizeObserved_)
        {
            // The app was restored while the minimize image was in flight.
            Finish();
            return false;
        }
    }
    if (!snowdesktop::animation::RuntimeAnimationsEnabled())
    {
        CompleteImmediately();
        return false;
    }

    const double now = nowMilliseconds;
    if (awaitingMinimizePresentation_)
    {
        if (!PrepareMinimizePresentation(now)) return IsActive();
        return false; // Preparation replaced this polling token with a timeline.
    }
    if (pendingExternalRestore_)
    {
        if (!IsIconic(sourceWindow_)) return true;
        pendingExternalRestore_ = false;
        animationStartTimeMs_ = now;
        auto callback = std::move(restoreCallback_);
        if (!Reverse(DockWindowTransitionDirection::Restore, std::move(callback)))
        {
            CompleteRestoreAfterRenderFailure();
            Finish();
        }
        return false;
    }
    if (preparingRestore_)
    {
        if (!PrepareRestoredWindow(now)) return IsActive();
        // The preparation poll owns a CPU frame token, but the restored
        // image now has its final geometry and can run independently.
        if (!ScheduleAnimationWake())
        {
            CompleteRestoreAfterRenderFailure();
            Finish();
        }
        return false;
    }
    if (awaitingRestoreVisibility_)
    {
        // This deadline covers every handoff branch, including ready geometry
        // with a failed uncloak and each asynchronous presentation phase.
        if (now >= restoreCleanupDeadlineMs_)
        {
            Finish();
            return false;
        }
        if (restorePresentationPhase_ == RestorePresentationPhase::AlignedImage)
        {
            const auto result = PresentationBarrierResult(now);
            if (!result) return true;
            presentationBarrier_.reset();
            restorePresentationPhase_ = RestorePresentationPhase::None;
            if (FAILED(*result))
            {
                Finish();
                return false;
            }
            // DwmUnregisterThumbnail is independent of our composition commit.
            // Keep the previous live relationships until their replacements
            // have been presented, while the native source remains cloaked.
            retiredWindowImage_.Reset();
            retiredGenieWindowImages_.clear();
            if (diagnosticCallback_) diagnosticCallback_(L"Dock restore realigned: image presented before source exposure");
        }
        if (restorePresentationPhase_ == RestorePresentationPhase::ExposedSource ||
            restorePresentationPhase_ == RestorePresentationPhase::TransparentOverlay)
        {
            const auto result = PresentationBarrierResult(now);
            if (!result) return true;
            const bool retiring = restorePresentationPhase_ == RestorePresentationPhase::TransparentOverlay;
            presentationBarrier_.reset();
            restorePresentationPhase_ = RestorePresentationPhase::None;
            if (retiring || FAILED(*result))
            {
                restorePresentationAcknowledged_ = retiring && SUCCEEDED(*result);
                Finish();
                return false;
            }
            restoreVisibleTimeMs_ = MonotonicTimeMilliseconds();
            return true;
        }
        if (restoreFadeStartTimeMs_ > 0.0)
        {
            const double fadeProgress = std::min(
                1.0,
                (now - restoreFadeStartTimeMs_) /
                    static_cast<double>(
                        kRestoreImageFadeDurationMs));
            if (!ApplyFrame(fadeProgress))
            {
                // The real window is already restored and activated. If the
                // retiring overlay fails to render, remove it immediately
                // instead of invoking another restore command.
                ActivateRestoredWindowForHandoff();
                Finish();
                return false;
            }
            if (fadeProgress < 1.0)
                return true;

            // Retain the transparent scene until its presentation is confirmed,
            // while widgets and the host message loop continue on this thread.
            if (BeginPresentationBarrier(RestorePresentationPhase::TransparentOverlay))
                return true;
            Finish();
            return false;
        }

        if (restoreVisibleTimeMs_ > 0.0)
        {
            if (now - restoreVisibleTimeMs_ <
                static_cast<double>(
                    kRestorePresentationDelayMs))
            {
                return true;
            }

            // IsIconic clearing precedes the first composed frame of some
            // applications. Hold the opaque shared image for one presentation,
            // then retire it without changing its final full-window geometry.
            ActivateRestoredWindowForHandoff();
            fromRect_ = windowRect_;
            toRect_ = windowRect_;
            animationFromOpacity_ = lastFrameOpacity_;
            animationToOpacity_ = 0;
            animationStartTimeMs_ = now;
            animationDurationMs_ = static_cast<double>(
                kRestoreImageFadeDurationMs);
            restoreFadeStartTimeMs_ = now;
            compositionTimelineActive_ = false;
            if (!ScheduleAnimationWake()) Finish();
            return false;
        }

        // 失败检查：恢复回调已执行，但窗口必须在清理超时内真正退出最小化。
        // 目标进程挂起（例如求解器无法处理 SW_RESTORE）时 IsIconic 会一直
        // 保持为真，此时必须中止动画而不是无限等待，否则过渡层会永久停留
        // 在窗口位置，表现为 Dock 卡死。
        RECT restoredFrame{};
        if (RestoreGeometryReady(restoredFrame))
        {
            if (!EqualRect(&restoredFrame, &windowRect_))
            {
                // An app can finish its native border/layout update during the
                // animation. Align the final live image instead of waiting for
                // the real window to return to the earlier, now stale bounds.
                windowRect_ = restoredFrame;
                fromRect_ = toRect_ = windowRect_;
                const RECT requestedHost = ResolveImageHostRect();
                RECT retainedHost{};
                UnionRect(&retainedHost, &imageHostRect_, &requestedHost);
                const auto safeHost = ResolveDockWindowNonFullscreenHostRect(
                    retainedHost, animationMonitorRects_);
                bool aligned = safeHost.has_value();
                if (aligned && !EqualRect(&imageHostRect_, &*safeHost))
                {
                    imageHostRect_ = *safeHost;
                    aligned = SetWindowPos(hwnd_, nullptr, imageHostRect_.left, imageHostRect_.top,
                        imageHostRect_.right - imageHostRect_.left,
                        imageHostRect_.bottom - imageHostRect_.top, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
                }
                if (aligned)
                {
                    retiredWindowImage_ = std::move(compositionWindowImage_);
                    retiredGenieWindowImages_ = std::move(genieWindowImages_);
                    ClearGenieStrips();
                    aligned = SUCCEEDED(compositionVisual_->RemoveAllVisuals()) && CreateSharedWindowImage();
                }
                hasLastFrame_ = false;
                compositionTimelineActive_ = false;
                if (aligned) aligned = ApplyFrame(1.0);
                if (!aligned || !BeginPresentationBarrier(RestorePresentationPhase::AlignedImage))
                {
                    CompleteRestoreAfterRenderFailure();
                    Finish();
                    return false;
                }
                if (diagnosticCallback_) diagnosticCallback_(L"Dock restore realigning: retaining live image until presentation");
                return true;
            }
            ActivateRestoredWindowForHandoff();
            // A failed cross-process release retains ownership; do not retire
            // the only visible image while its source is still cloaked.
            if (!sourceCloaked_)
            {
                if (!BeginPresentationBarrier(RestorePresentationPhase::ExposedSource))
                {
                    Finish();
                    return false;
                }
            }
            return true;
        }
        if (now >= restoreCleanupDeadlineMs_)
        {
            Finish();
            return false;
        }
        return true;
    }

    const double progress = std::min(
        1.0,
        (now - animationStartTimeMs_) /
            std::max(1.0, animationDurationMs_));
    if (!ApplyFrame(progress))
    {
        CompleteRestoreAfterRenderFailure();
        Finish();
        return false;
    }
    if (progress < 1.0)
        return true;

    if (direction_ == DockWindowTransitionDirection::Minimize && !IsIconic(sourceWindow_) &&
        now < minimizeCleanupDeadlineMs_)
    {
        // Posting SC_MINIMIZE acknowledges dispatch, not the application's
        // state change. Keep the source cloaked until that handoff
        // completes; some browser/toolkit UI threads process it late.
        return true;
    }

    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_)
    {
        // The source has been restoring underneath the visual timeline. Keep
        // its full-size image until the final source frame can be exposed.
        awaitingRestoreVisibility_ = true;
        restoreCleanupDeadlineMs_ =
            now + static_cast<double>(
                kRestoreCleanupTimeoutMs);
        return true;
    }
    Finish();
    return false;
}

void DockWindowTransition::RequestRestoreForAnimation()
{
    if (direction_ != DockWindowTransitionDirection::Restore ||
        !restoreCallback_ || restoreRequested_)
        return;
    restoreRequested_ = true;
    preparingRestore_ = true;
    restoreGeometryStableTimeMs_ = 0.0;
    restoreCleanupDeadlineMs_ = MonotonicTimeMilliseconds() + kRestoreCleanupTimeoutMs;
    // Request the native restore while the source is cloaked and our first
    // (collapsed, transparent) frame is already committed. This gives native
    // geometry and application painting the whole Genie timeline to settle.
    restoreCallback_(sourceWindow_, DockWindowRestoreTransitionPhase::RequestRestore);
}

bool DockWindowTransition::RestoreGeometryReady(RECT& frame) const
{
    if (!sourceWindow_ || IsIconic(sourceWindow_) || !IsWindowVisible(sourceWindow_) ||
        !ResolveVisibleWindowRect(sourceWindow_, frame))
        return false;
    if (!UsesCompositionImage()) return false;
    RECT actualWindow{};
    if (!GetWindowRect(sourceWindow_, &actualWindow) || !IsUsableRect(actualWindow))
        return false;
    // Win10 can retain the legacy resize borders in an iconic shared image,
    // then report only the visible frame on restore. Image-size differences
    // are valid: preparation rebinds the live image at the current geometry.
    // Reject only the recognizable temporary iconic bar after IsIconic clears.
    const UINT dpi = GetDpiForWindow(sourceWindow_);
    const int barHeight = GetSystemMetricsForDpi(SM_CYMINIMIZED, dpi ? dpi : 96);
    if (actualWindow.bottom - actualWindow.top <= barHeight &&
        windowRect_.bottom - windowRect_.top > barHeight)
        return false;
    // DWM's frame bounds may lag the native HWND during restore. A usable
    // current HWND rectangle is sufficient while the shared image catches up.
    if (frame.left < actualWindow.left || frame.top < actualWindow.top ||
        frame.right > actualWindow.right || frame.bottom > actualWindow.bottom)
        frame = actualWindow;
    return true;
}

bool DockWindowTransition::PrepareRestoredWindow(double now)
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "restore.prepare");
    if (restorePresentationPhase_ == RestorePresentationPhase::PreparedImage)
    {
        const auto result = PresentationBarrierResult(now);
        if (!result) return false;
        presentationBarrier_.reset();
        restorePresentationPhase_ = RestorePresentationPhase::None;
        if (FAILED(*result))
        {
            CompleteRestoreAfterRenderFailure();
            Finish();
            return false;
        }
        animationStartTimeMs_ = MonotonicTimeMilliseconds();
        preparingRestore_ = false;
        if (diagnosticCallback_) diagnosticCallback_(L"Dock restore prepared: full-geometry before animation");
        return true;
    }
    RECT frame{};
    if (!RestoreGeometryReady(frame))
    {
        restoreGeometryStableTimeMs_ = 0.0;
    }
    else if (restoreGeometryStableTimeMs_ <= 0.0 || !EqualRect(&frame, &restoreGeometryRect_))
    {
        restoreGeometryRect_ = frame;
        restoreGeometryStableTimeMs_ = now;
    }
    else if (now - restoreGeometryStableTimeMs_ >= kRestorePresentationDelayMs)
    {
        // Resolve both target geometry and source crop from the actual restored
        // window. WINDOWPLACEMENT and a minimized HWND cannot supply this crop.
        windowRect_ = frame;
        toRect_ = windowRect_;
        if (effect_ == 2) fromRect_ = toRect_;
        // A reversed Genie's interpolated frame is only a timing proxy: its
        // pulled edge can already reach the Dock outside that rectangle. Keep
        // the full deformation envelope when rebinding the restored image.
        const RECT requestedHost = ResolveImageHostRect();
        const auto safeHost = ResolveDockWindowNonFullscreenHostRect(requestedHost, animationMonitorRects_);
        bool prepared = safeHost.has_value();
        const wchar_t* rebindStage = L"host-bounds";
        HRESULT rebindResult = prepared ? S_OK : E_INVALIDARG;
        if (prepared)
        {
            rebindStage = L"host-position";
            imageHostRect_ = *safeHost;
            prepared = SetWindowPos(hwnd_, nullptr, imageHostRect_.left, imageHostRect_.top,
                imageHostRect_.right - imageHostRect_.left,
                imageHostRect_.bottom - imageHostRect_.top,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
            if (!prepared)
            {
                const DWORD error = GetLastError();
                rebindResult = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
        }
        if (prepared && compositionSharedWindowActive_)
        {
            rebindStage = L"image-detach";
            ClearGenieStrips();
            compositionWindowImage_.Reset();
            rebindResult = compositionVisual_->RemoveAllVisuals();
            prepared = SUCCEEDED(rebindResult);
            if (prepared)
            {
                rebindStage = L"shared-image";
                prepared = CreateSharedWindowImage();
                rebindResult = imageResult_;
            }
        }
        hasLastFrame_ = false;
        compositionTimelineActive_ = false;
        if (prepared)
        {
            rebindStage = L"first-frame";
            prepared = ApplyFrame(0.0);
            if (!prepared) rebindResult = E_FAIL;
        }
        if (!prepared)
        {
            if (diagnosticCallback_)
            {
                wchar_t diagnostic[512]{};
                swprintf_s(diagnostic,
                    L"Dock animation aborted: stage=restore-rebind substage=%ls hr=0x%08X "
                    L"window=[%ld,%ld,%ld,%ld] host=[%ld,%ld,%ld,%ld] image=%ldx%ld collapse=%.4f",
                    rebindStage, static_cast<unsigned>(rebindResult),
                    windowRect_.left, windowRect_.top, windowRect_.right, windowRect_.bottom,
                    requestedHost.left, requestedHost.top, requestedHost.right, requestedHost.bottom,
                    compositionImageSize_.cx, compositionImageSize_.cy, collapseFrom_);
                diagnosticCallback_(diagnostic);
            }
            CompleteRestoreAfterRenderFailure();
            Finish();
            return false;
        }
        if (!BeginPresentationBarrier(RestorePresentationPhase::PreparedImage))
        {
            CompleteRestoreAfterRenderFailure();
            Finish();
        }
        return false;
    }
    if (now >= restoreCleanupDeadlineMs_)
    {
        if (diagnosticCallback_) diagnosticCallback_(L"Dock animation aborted: stage=restore-geometry-timeout");
        CompleteRestoreAfterRenderFailure();
        Finish();
    }
    return false;
}

void DockWindowTransition::
CompleteRestoreAfterRenderFailure()
{
    if (direction_ !=
            DockWindowTransitionDirection::Restore ||
        !restoreCallback_ ||
        !sourceWindow_ ||
        !IsWindow(sourceWindow_))
        return;
    RestoreCallback callback =
        std::move(restoreCallback_);
    callback(
        sourceWindow_,
        DockWindowRestoreTransitionPhase::
            FallbackWithoutAnimation);
}

void DockWindowTransition::
ActivateRestoredWindowForHandoff()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "source.activate");
    if (!restoreCallback_ || !sourceWindow_ ||
        !IsWindow(sourceWindow_) || IsIconic(sourceWindow_))
        return;
    ReleaseSourceCloak();
    if (sourceCloaked_ || restoreActivated_) return;
    restoreActivated_ = true;
    restoreCallback_(
        sourceWindow_,
        DockWindowRestoreTransitionPhase::ActivateRestored);
}

void DockWindowTransition::ReleaseSourceCloak()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "source.expose");
    if (!sourceCloaked_) return;
    if (sourceCloakWindow_ && IsWindow(sourceCloakWindow_))
    {
        const HRESULT hr = sourceRelease_ ? sourceRelease_(sourceCloakWindow_, hwnd_) :
            snowdesktop::dock_source_cloak::Release(sourceCloakWindow_, hwnd_);
        if (FAILED(hr))
        {
            if (diagnosticCallback_) diagnosticCallback_(L"Dock source uncloak failed");
            return;
        }
    }
    sourceCloaked_ = false;
    sourceCloakWindow_ = nullptr;
}

void DockWindowTransition::Finish()
{
    snowdesktop::performance::Scope performanceScope("dock.transition", "scene.retire");
    ClearExternalPresentationWait();
    awaitingMinimizePresentation_ = false;
    pendingExternalRestore_ = false;
    minimizeSourceWindowRect_ = {};
    minimizeRequest_ = {};
    presentationBarrier_.reset();
    restorePresentationPhase_ = RestorePresentationPhase::None;
    presentationBarrierDeadlineMs_ = 0.0;
    restorePresentationAcknowledged_ = false;
    externalMinimize_ = false;
    externalMinimizeObserved_ = false;
    externalMinimizeDeadline_ = 0;
    externalMinimizeObservationDeadline_ = 0;
    minimizeObserved_ = false;
    minimizeCleanupDeadlineMs_ = 0.0;
    if (animationScheduler_ && animationToken_)
        animationScheduler_->Cancel(animationToken_);
    animationToken_ = 0;
    HWND transitionWindow = hwnd_;
    // Hide and detach without waiting for the compositor on the UI thread.
    // The preallocated owner retains all old visuals and shared thumbnails.
    if (compositionEffect_) compositionEffect_->SetOpacity(0.0f);
    if (compositionTarget_) compositionTarget_->SetRoot(nullptr);
    if (compositionImageActive_ && compositionDevice_) compositionDevice_->Commit();
    if (presenting_ && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-before-hide");
    if (transitionWindow)
    {
        ShowWindow(
            transitionWindow, SW_HIDE);
        SetWindowRgn(
            transitionWindow, nullptr, FALSE);
    }
    if (presenting_ && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: overlay-after-hide");
    const bool wasPresenting = std::exchange(presenting_, false);
    hasOcclusionRegion_ = false;
    occlusionRects_.clear();
    occlusionHostBounds_ = {};
    if (wasPresenting && presentationCallback_)
        presentationCallback_(nullptr);
    if (wasPresenting && diagnosticCallback_)
        diagnosticCallback_(L"Dock taskbar phase: after-dock-layer-restore");
    ReleaseSourceCloak();
    if (sceneRetirement_)
    {
        auto state = std::move(sceneRetirement_);
        state->device = compositionDevice_;
        state->target = std::move(compositionTarget_);
        state->visual = std::move(compositionVisual_);
        state->scale = std::move(compositionScaleTransform_);
        state->effect = std::move(compositionEffect_);
        state->clip = std::move(compositionClip_);
        state->strips = std::move(genieStrips_);
        state->triangles = std::move(attractionTriangles_);
        state->image = std::move(compositionWindowImage_);
        state->images = std::move(genieWindowImages_);
        state->previousImage = std::move(retiredWindowImage_);
        state->previousImages = std::move(retiredGenieWindowImages_);
        state->window = transitionWindow;
        state->token = transitionWindow ? GetPropW(transitionWindow,
            snowdesktop::dock_source_cloak::HostTokenProperty) : nullptr;
        if (sourceCloaked_)
        {
            state->source = sourceCloakWindow_;
            state->owner = snowdesktop::dock_source_cloak::ReadOwner(state->source);
            if (state->owner.window == transitionWindow)
                SetPropW(state->source, snowdesktop::dock_source_cloak::RecoveryProperty, state->owner.token);
        }
        if (transitionWindow)
        {
            // Transfer the UI reference to this HWND before the worker starts.
            // An early NCDESTROY releases only that reference.
            state->references.fetch_add(1, std::memory_order_relaxed);
            SetWindowLongPtrW(transitionWindow, 0, reinterpret_cast<LONG_PTR>(state.get()));
            SetWindowLongPtrW(transitionWindow, GWLP_USERDATA, 0);
        }
        hwnd_ = nullptr;
        sourceCloaked_ = false;
        sourceCloakWindow_ = nullptr;
        const PTP_WORK work = state->work;
        state.release();
        SubmitThreadpoolWork(work);
    }
    compositionSharedWindowActive_ = false;
    compositionImageSize_ = {};
    compositionSourceRegion_ = {};
    compositionImageActive_ = false;
    compositionTimelineActive_ = false;
    sourceWindow_ = nullptr;
    sourceProcess_ = sourceThread_ = 0;
    fromRect_ = {};
    toRect_ = {};
    windowRect_ = {};
    dockRect_ = {};
    imageHostRect_ = {};
    animationMonitorRects_.clear();
    lastFrameRect_ = {};
    lastFrameOpacity_ = 0;
    hasLastFrame_ = false;
    animationStartTimeMs_ = 0.0;
    animationDurationMs_ =
        static_cast<double>(
            kAnimationDurationMs);
    restoreCleanupDeadlineMs_ = 0.0;
    restoreVisibleTimeMs_ = 0.0;
    restoreFadeStartTimeMs_ = 0.0;
    animationFromOpacity_ = 255;
    animationToOpacity_ = 0;
    awaitingRestoreVisibility_ = false;
    preparingRestore_ = false;
    restoreRequested_ = false;
    restoreActivated_ = false;
    restoreGeometryStableTimeMs_ = 0.0;
    restoreGeometryRect_ = {};
    effect_ = 1;
    collapseFrom_ = 0.0;
    collapseTo_ = 1.0;
    lastCollapse_ = 0.0;
    lastGenieCollapse_ = -1.0;
    restoreCallback_ = {};
}

void DockWindowTransition::Cancel()
{
    if (helperRouting_ && (remoteAnimation_ || snowdesktop::window_support::WindowSupport().HelperSnapshot().animationActive))
    {
        snowdesktop::window_support::protocol::Request request;
        request.command = snowdesktop::window_support::protocol::Command::CancelAnimation;
        snowdesktop::window_support::WindowSupport().Submit(request);
        remoteAnimation_.reset();
    }
    // Cancellation used to run after the caller had already dispatched its
    // native minimize. Preserve that accepted operation when only its first
    // frame is pending; explicit reversal uses Finish to withdraw that intent.
    if ((awaitingMinimizePresentation_ && minimizeRequest_) || pendingExternalRestore_) CompleteImmediately();
    else Finish();
}

void DockWindowTransition::CompleteImmediately()
{
    if (helperRouting_ && (remoteAnimation_ || snowdesktop::window_support::WindowSupport().HelperSnapshot().animationActive))
    {
        snowdesktop::window_support::protocol::Request request;
        request.command = snowdesktop::window_support::protocol::Command::CompleteAnimation;
        snowdesktop::window_support::WindowSupport().Submit(request);
        remoteAnimation_.reset();
    }
    auto minimize = std::move(minimizeRequest_);
    const HWND source = SourceIdentityCurrent() ? sourceWindow_ : nullptr;
    auto externalRestore = pendingExternalRestore_ ? std::move(restoreCallback_) : RestoreCallback{};
    // A pending first-frame minimize falls back after removing its overlay.
    // A restore already starts underneath it, so complete that native handoff
    // explicitly before removing the presentation window.
    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_ && sourceWindow_ && IsWindow(sourceWindow_))
    {
        if (IsIconic(sourceWindow_))
            CompleteRestoreAfterRenderFailure();
        else
            ActivateRestoredWindowForHandoff();
    }
    Finish();
    if (minimize && source) minimize(source);
    if (externalRestore && source)
    {
        // The host's existing asynchronous restore route queues behind the
        // original CBT operation, retaining maximized placement and fallbacks.
        externalRestore(source, DockWindowRestoreTransitionPhase::RequestRestore);
        externalRestore(source, DockWindowRestoreTransitionPhase::ActivateRestored);
    }
}

bool DockWindowTransition::IsActive() const
{
    if (helperRouting_)
    {
        if (remoteAnimation_ && remoteAnimation_->pending && GetTickCount64() - remoteAnimation_->acceptedAt < 2500) return true;
        const auto snapshot = snowdesktop::window_support::WindowSupport().HelperSnapshot();
        if (snapshot.animationActive && snowdesktop::window_support::protocol::Current(snapshot.animationTarget)) return true;
    }
    return sourceWindow_ != nullptr && compositionSharedWindowActive_;
}

bool DockWindowTransition::IsActiveFor(
    HWND window) const
{
    if (!IsActive() ||
        !window ||
        !IsWindow(window))
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    if (helperRouting_)
    {
        if (remoteAnimation_ && remoteAnimation_->pending && GetTickCount64() - remoteAnimation_->acceptedAt < 2500 && remoteAnimation_->source == root) return true;
        const auto snapshot = snowdesktop::window_support::WindowSupport().HelperSnapshot();
        if (snapshot.animationActive && snowdesktop::window_support::protocol::Current(snapshot.animationTarget) &&
            snowdesktop::window_support::protocol::Window(snapshot.animationTarget) == root) return true;
    }
    return sourceWindow_ == root;
}

DockWindowTransitionDirection
DockWindowTransition::GetDirection() const
{
    if (helperRouting_)
    {
        if (remoteAnimation_ && remoteAnimation_->pending) return remoteAnimation_->direction;
        const auto snapshot = snowdesktop::window_support::WindowSupport().HelperSnapshot();
        if (snapshot.animationActive) return snapshot.direction == 1 ? DockWindowTransitionDirection::Restore : DockWindowTransitionDirection::Minimize;
    }
    return pendingExternalRestore_ ? DockWindowTransitionDirection::Restore : direction_;
}

HWND DockWindowTransition::GetPresentationWindow() const noexcept
{
    if (helperRouting_)
    {
        const auto snapshot = snowdesktop::window_support::WindowSupport().HelperSnapshot();
        if (snapshot.animationActive) return reinterpret_cast<HWND>(static_cast<UINT_PTR>(snapshot.presentation));
    }
    return presenting_ ? hwnd_ : nullptr;
}
bool DockWindowTransition::IsPresentationWindow(HWND window) const noexcept
{ return window && (window == hwnd_ || window == GetPresentationWindow()); }

LRESULT CALLBACK DockWindowTransition::WindowProc(
    HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<DockWindowTransition*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(
            lParam);
        self = static_cast<DockWindowTransition*>(
            create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    auto* retired = reinterpret_cast<RetiredScene*>(GetWindowLongPtrW(window, 0));
    if (message == kRetireSceneWindow && !self && retired &&
        reinterpret_cast<LPARAM>(retired) == lParam &&
        retired->completed.load(std::memory_order_acquire) &&
        GetPropW(window, snowdesktop::dock_source_cloak::HostTokenProperty) == reinterpret_cast<HANDLE>(wParam))
    {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_NCDESTROY && retired)
    {
        SetWindowLongPtrW(window, 0, 0);
        retired->windowDestroyed.store(true, std::memory_order_release);
        retired->Release();
    }

    switch (message)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (self)
        {
            self->ReleaseSourceCloak();
            self->hwnd_ = nullptr;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(
        window, message, wParam, lParam);
}
