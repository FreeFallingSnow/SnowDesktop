#include "dock_window_transition.h"

#include <algorithm>
#include <cmath>

namespace
{

constexpr wchar_t kDockWindowTransitionClassName[] =
    L"SnowDesktopDockWindowTransition";

bool IsUsableRect(const RECT& rect)
{
    return rect.right - rect.left > 1 &&
        rect.bottom - rect.top > 1;
}

bool SystemWindowAnimationsEnabled()
{
    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) ||
        !compositionEnabled)
        return false;

    ANIMATIONINFO animationInfo{ sizeof(animationInfo) };
    if (SystemParametersInfoW(
            SPI_GETANIMATION, sizeof(animationInfo),
            &animationInfo, 0) &&
        animationInfo.iMinAnimate == 0)
        return false;

    BOOL clientAreaAnimation = TRUE;
    if (SystemParametersInfoW(
            SPI_GETCLIENTAREAANIMATION, 0,
            &clientAreaAnimation, 0) &&
        !clientAreaAnimation)
        return false;
    return true;
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

RECT ResolveDockWindowSnapshotHostRect(
    const RECT& from, const RECT& to) noexcept
{
    return {
        std::min(from.left, to.left),
        std::min(from.top, to.top),
        std::max(from.right, to.right),
        std::max(from.bottom, to.bottom)
    };
}

SIZE ConstrainDockWindowSnapshotSize(
    SIZE source, LONG maximumWidth,
    LONG maximumHeight) noexcept
{
    if (source.cx <= 0 || source.cy <= 0 ||
        maximumWidth <= 0 || maximumHeight <= 0)
        return {};
    const double scale = std::min({
        1.0,
        static_cast<double>(maximumWidth) /
            static_cast<double>(source.cx),
        static_cast<double>(maximumHeight) /
            static_cast<double>(source.cy)
    });
    return {
        std::max<LONG>(1, static_cast<LONG>(
            std::lround(source.cx * scale))),
        std::max<LONG>(1, static_cast<LONG>(
            std::lround(source.cy * scale)))
    };
}

DockWindowTransition::~DockWindowTransition()
{
    Cancel();
    activeSnapshotBitmap_.Reset();
    snapshotRenderTarget_.Reset();
    d2dFactory_.Reset();
    if (hwnd_)
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (instance_)
        UnregisterClassW(
            kDockWindowTransitionClassName, instance_);
}

bool DockWindowTransition::Initialize(HINSTANCE instance)
{
    instance_ = instance;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName =
        kDockWindowTransitionClassName;
    const bool registered =
        RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    if (registered && EnsureWindow())
        EnsureSnapshotRenderer();
    return registered;
}

bool DockWindowTransition::EnsureWindow()
{
    if (hwnd_ && IsWindow(hwnd_))
        return true;
    if (!instance_)
        return false;

    hwnd_ = CreateWindowExW(
        kDockWindowTransitionExStyle,
        kDockWindowTransitionClassName,
        L"Dock Window Transition",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!hwnd_)
        return false;
    if (!SetLayeredWindowAttributes(
            hwnd_, 0, 255, LWA_ALPHA))
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

bool DockWindowTransition::StartMinimize(
    HWND sourceWindow, RECT dockRect)
{
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Minimize,
        {});
}

bool DockWindowTransition::PrimeMinimizeSnapshot(
    HWND sourceWindow)
{
    if (IsActive() ||
        !SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
    RECT windowRect{};
    if (!ResolveVisibleWindowRect(
            sourceWindow, windowRect))
        return false;
    lastVisibleRects_[sourceWindow] = windowRect;
    return PrepareSnapshot(
        sourceWindow, windowRect,
        DockWindowTransitionDirection::Minimize,
        false) != nullptr;
}

bool DockWindowTransition::StartRestore(
    HWND sourceWindow, RECT dockRect,
    RestoreCallback restoreCallback)
{
    if (!restoreCallback)
        return false;
    return Start(
        sourceWindow, dockRect,
        DockWindowTransitionDirection::Restore,
        std::move(restoreCallback));
}

bool DockWindowTransition::Start(
    HWND sourceWindow, RECT dockRect,
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback)
{
    if (!SystemWindowAnimationsEnabled() ||
        !sourceWindow || !IsWindow(sourceWindow) ||
        !IsUsableRect(dockRect))
        return false;

    HWND root = GetAncestor(sourceWindow, GA_ROOT);
    sourceWindow = root ? root : sourceWindow;
    const auto startAction =
        ResolveDockWindowTransitionStartAction(
            IsActive(),
            sourceWindow_ == sourceWindow,
            direction_ == direction);
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
        return Reverse(
            direction,
            std::move(restoreCallback));
    }

    Cancel();
    sourceWindow_ = sourceWindow;
    direction_ = direction;
    restoreCallback_ = std::move(restoreCallback);

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
        if (!ResolveRestoreWindowRect(sourceWindow_, windowRect))
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
    animationFromOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 0.0);
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ =
        static_cast<double>(
            kAnimationDurationMs);

    const CachedSnapshot* snapshot =
        PrepareSnapshot(
            sourceWindow_, windowRect,
            direction_, true);
    if (!EnsureWindow())
    {
        Cancel();
        return false;
    }

    bool snapshotAvailable = false;
    if (snapshot &&
        EnsureSnapshotRenderer())
    {
        snapshotAvailable =
            CreateActiveSnapshotBitmap(*snapshot);
    }

    bool liveThumbnailAvailable = false;
    if (!snapshotAvailable)
    {
        liveThumbnailAvailable =
            SUCCEEDED(DwmRegisterThumbnail(
                hwnd_, sourceWindow_, &thumbnail_)) &&
            thumbnail_;
    }
    surface_ =
        ResolveDockWindowTransitionSurface(
            snapshotAvailable,
            liveThumbnailAvailable);
    if (surface_ ==
        DockWindowTransitionSurface::None)
    {
        Cancel();
        return false;
    }

    snapshotHostRect_ =
        surface_ ==
            DockWindowTransitionSurface::Snapshot
        ? ResolveDockWindowSnapshotHostRect(
            fromRect_, toRect_)
        : fromRect_;
    const int hostWidth = std::max(
        1L, snapshotHostRect_.right -
            snapshotHostRect_.left);
    const int hostHeight = std::max(
        1L, snapshotHostRect_.bottom -
            snapshotHostRect_.top);
    SetWindowPos(
        hwnd_, HWND_TOPMOST,
        snapshotHostRect_.left,
        snapshotHostRect_.top,
        hostWidth, hostHeight,
        SWP_NOACTIVATE);

    if (!ApplyFrame(0.0))
    {
        Cancel();
        return false;
    }

    const BOOL disableTransitions = TRUE;
    if (FAILED(DwmSetWindowAttribute(
            sourceWindow_,
            DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions,
            sizeof(disableTransitions))))
    {
        Cancel();
        return false;
    }
    nativeTransitionsDisabled_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    if (RequiresDockWindowTransitionCompositionBarrier(direction_) &&
        FAILED(DwmFlush()))
    {
        Cancel();
        return false;
    }

    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    awaitingRestoreVisibility_ = false;
    restoreCleanupDeadlineMs_ = 0.0;
    if (!SetCoalescableTimer(
            hwnd_, kAnimationTimerId,
            kAnimationTimerIntervalMs, nullptr,
            TIMERV_NO_COALESCING))
    {
        Cancel();
        return false;
    }
    return true;
}

bool DockWindowTransition::Reverse(
    DockWindowTransitionDirection direction,
    RestoreCallback restoreCallback)
{
    if (!IsActive() ||
        !hasLastFrame_ ||
        awaitingRestoreVisibility_)
        return false;

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
    const double remainingRatio = std::clamp(
        maximumEdgeDistance(
            currentFrame, targetFrame) /
            fullDistance,
        0.0, 1.0);

    direction_ = direction;
    fromRect_ = currentFrame;
    toRect_ = targetFrame;
    animationFromOpacity_ =
        lastFrameOpacity_;
    animationToOpacity_ =
        ResolveDockWindowTransitionOpacity(
            direction_, 1.0);
    animationDurationMs_ = std::clamp(
        static_cast<double>(
            kAnimationDurationMs) *
            remainingRatio,
        static_cast<double>(
            kMinimumReverseDurationMs),
        static_cast<double>(
            kAnimationDurationMs));
    restoreCallback_ =
        std::move(restoreCallback);
    animationStartTimeMs_ =
        MonotonicTimeMilliseconds();
    restoreCleanupDeadlineMs_ = 0.0;
    awaitingRestoreVisibility_ = false;
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

    if ((placement.flags & WPF_RESTORETOMAXIMIZED) != 0)
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

bool DockWindowTransition::CaptureSnapshot(
    HWND window, const RECT& sourceRect,
    CachedSnapshot& snapshot) const
{
    if (!window || !IsWindow(window) ||
        !IsUsableRect(sourceRect))
        return false;

    const SIZE sourceSize{
        sourceRect.right - sourceRect.left,
        sourceRect.bottom - sourceRect.top
    };
    const SIZE pixelSize =
        ConstrainDockWindowSnapshotSize(sourceSize);
    if (pixelSize.cx <= 0 || pixelSize.cy <= 0)
        return false;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc)
        return false;
    HDC snapshotDc = CreateCompatibleDC(screenDc);
    if (!snapshotDc)
    {
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize =
        sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth =
        pixelSize.cx;
    bitmapInfo.bmiHeader.biHeight =
        -pixelSize.cy;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bitmapBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        screenDc, &bitmapInfo,
        DIB_RGB_COLORS, &bitmapBits,
        nullptr, 0);
    if (!bitmap || !bitmapBits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(snapshotDc);
        ReleaseDC(nullptr, screenDc);
        return false;
    }

    HGDIOBJ previousBitmap =
        SelectObject(snapshotDc, bitmap);
    SetStretchBltMode(snapshotDc, HALFTONE);
    SetBrushOrgEx(snapshotDc, 0, 0, nullptr);
    const BOOL captured = StretchBlt(
        snapshotDc,
        0, 0, pixelSize.cx, pixelSize.cy,
        screenDc,
        sourceRect.left, sourceRect.top,
        sourceSize.cx, sourceSize.cy,
        SRCCOPY | CAPTUREBLT);
    GdiFlush();

    if (previousBitmap)
        SelectObject(snapshotDc, previousBitmap);
    if (captured)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(
            window, &processId);
        snapshot.processId = processId;
        snapshot.pixelSize = pixelSize;
        snapshot.sourceRect = sourceRect;
        snapshot.capturedTick =
            GetTickCount64();
        snapshot.lastUsedTick =
            snapshot.capturedTick;
        const auto* firstPixel =
            static_cast<const std::uint32_t*>(
                bitmapBits);
        snapshot.pixels.assign(
            firstPixel,
            firstPixel +
                static_cast<std::size_t>(
                    pixelSize.cx) *
                static_cast<std::size_t>(
                    pixelSize.cy));
    }

    DeleteObject(bitmap);
    DeleteDC(snapshotDc);
    ReleaseDC(nullptr, screenDc);
    return captured != FALSE &&
        !snapshot.pixels.empty();
}

void DockWindowTransition::PurgeSnapshotCache()
{
    for (auto iterator = snapshotCache_.begin();
         iterator != snapshotCache_.end();)
    {
        DWORD processId = 0;
        if (!IsWindow(iterator->first))
        {
            lastVisibleRects_.erase(iterator->first);
            iterator = snapshotCache_.erase(iterator);
            continue;
        }
        GetWindowThreadProcessId(
            iterator->first, &processId);
        if (!processId ||
            processId != iterator->second.processId)
        {
            lastVisibleRects_.erase(iterator->first);
            iterator = snapshotCache_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

const DockWindowTransition::CachedSnapshot*
DockWindowTransition::PrepareSnapshot(
    HWND window, const RECT& sourceRect,
    DockWindowTransitionDirection direction,
    bool allowFreshMinimizeSnapshot)
{
    PurgeSnapshotCache();
    if (direction ==
        DockWindowTransitionDirection::Minimize)
    {
        const ULONGLONG now = GetTickCount64();
        const auto primed =
            snapshotCache_.find(window);
        if (allowFreshMinimizeSnapshot &&
            primed != snapshotCache_.end() &&
            !primed->second.pixels.empty() &&
            EqualRect(
                &primed->second.sourceRect,
                &sourceRect) != FALSE &&
            now >= primed->second.capturedTick &&
            now - primed->second.capturedTick <=
                kPrimedSnapshotLifetimeMs)
        {
            primed->second.lastUsedTick = now;
            return &primed->second;
        }

        CachedSnapshot captured;
        if (!CaptureSnapshot(
                window, sourceRect, captured))
            return nullptr;

        std::size_t cachedBytes = 0;
        for (const auto& [cachedWindow, entry] :
             snapshotCache_)
        {
            if (cachedWindow != window)
                cachedBytes +=
                    entry.pixels.size() *
                    sizeof(std::uint32_t);
        }
        const std::size_t capturedBytes =
            captured.pixels.size() *
            sizeof(std::uint32_t);
        while (!snapshotCache_.empty() &&
            ((!snapshotCache_.contains(window) &&
              snapshotCache_.size() >=
                  kMaximumCachedSnapshots) ||
             cachedBytes + capturedBytes >
                 kMaximumCachedSnapshotBytes))
        {
            auto oldest =
                snapshotCache_.end();
            for (auto iterator =
                     snapshotCache_.begin();
                 iterator !=
                     snapshotCache_.end();
                 ++iterator)
            {
                if (iterator->first == window)
                    continue;
                if (oldest ==
                        snapshotCache_.end() ||
                    iterator->second.
                            lastUsedTick <
                        oldest->second.
                            lastUsedTick)
                    oldest = iterator;
            }
            if (oldest == snapshotCache_.end())
                break;
            cachedBytes -=
                oldest->second.pixels.size() *
                sizeof(std::uint32_t);
            lastVisibleRects_.erase(
                oldest->first);
            snapshotCache_.erase(oldest);
        }
        auto [iterator, inserted] =
            snapshotCache_.insert_or_assign(
                window, std::move(captured));
        (void)inserted;
        return &iterator->second;
    }

    const auto cached =
        snapshotCache_.find(window);
    if (cached == snapshotCache_.end() ||
        cached->second.pixels.empty())
        return nullptr;
    cached->second.lastUsedTick =
        GetTickCount64();
    return &cached->second;
}

bool DockWindowTransition::EnsureSnapshotRenderer()
{
    if (!hwnd_ || !IsWindow(hwnd_))
        return false;
    if (!d2dFactory_)
    {
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                d2dFactory_.ReleaseAndGetAddressOf())))
            return false;
    }
    if (snapshotRenderTarget_)
        return true;

    const D2D1_RENDER_TARGET_PROPERTIES
        renderProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_HARDWARE,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_IGNORE));
    const D2D1_HWND_RENDER_TARGET_PROPERTIES
        windowProperties =
            D2D1::HwndRenderTargetProperties(
                hwnd_, D2D1::SizeU(1, 1),
                kDockWindowSnapshotPresentOptions);
    HRESULT result =
        d2dFactory_->CreateHwndRenderTarget(
            renderProperties,
            windowProperties,
            snapshotRenderTarget_.
                ReleaseAndGetAddressOf());
    if (FAILED(result))
    {
        const auto fallbackProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_IGNORE));
        result =
            d2dFactory_->CreateHwndRenderTarget(
                fallbackProperties,
                windowProperties,
                snapshotRenderTarget_.
                    ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result) &&
        snapshotRenderTarget_)
    {
        snapshotRenderTarget_->SetDpi(
            kDockWindowSnapshotRenderDpi,
            kDockWindowSnapshotRenderDpi);
        return true;
    }
    return false;
}

bool DockWindowTransition::CreateActiveSnapshotBitmap(
    const CachedSnapshot& snapshot)
{
    activeSnapshotBitmap_.Reset();
    if (!snapshotRenderTarget_ ||
        snapshot.pixelSize.cx <= 0 ||
        snapshot.pixelSize.cy <= 0 ||
        snapshot.pixels.empty())
        return false;

    const D2D1_BITMAP_PROPERTIES properties =
        D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_IGNORE),
            kDockWindowSnapshotRenderDpi,
            kDockWindowSnapshotRenderDpi);
    return SUCCEEDED(
        snapshotRenderTarget_->CreateBitmap(
            D2D1::SizeU(
                static_cast<UINT32>(
                    snapshot.pixelSize.cx),
                static_cast<UINT32>(
                    snapshot.pixelSize.cy)),
            snapshot.pixels.data(),
            static_cast<UINT32>(
                snapshot.pixelSize.cx *
                sizeof(std::uint32_t)),
            properties,
            activeSnapshotBitmap_.
                ReleaseAndGetAddressOf())) &&
        activeSnapshotBitmap_;
}

bool DockWindowTransition::DrawSnapshotFrame(
    const RECT& destinationRect)
{
    if (!snapshotRenderTarget_ ||
        !activeSnapshotBitmap_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return false;
    RECT client{};
    if (!GetClientRect(hwnd_, &client))
        return false;
    const UINT32 width = static_cast<UINT32>(
        std::max(1L, client.right - client.left));
    const UINT32 height = static_cast<UINT32>(
        std::max(1L, client.bottom - client.top));
    const D2D1_SIZE_U currentSize =
        snapshotRenderTarget_->GetPixelSize();
    if ((currentSize.width != width ||
            currentSize.height != height) &&
        FAILED(snapshotRenderTarget_->Resize(
            D2D1::SizeU(width, height))))
        return false;

    snapshotRenderTarget_->BeginDraw();
    snapshotRenderTarget_->SetTransform(
        D2D1::Matrix3x2F::Identity());
    snapshotRenderTarget_->DrawBitmap(
        activeSnapshotBitmap_.Get(),
        D2D1::RectF(
            static_cast<float>(
                destinationRect.left),
            static_cast<float>(
                destinationRect.top),
            static_cast<float>(
                destinationRect.right),
            static_cast<float>(
                destinationRect.bottom)),
        1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    const HRESULT result =
        snapshotRenderTarget_->EndDraw();
    ValidateRect(hwnd_, nullptr);
    if (result == D2DERR_RECREATE_TARGET)
    {
        activeSnapshotBitmap_.Reset();
        snapshotRenderTarget_.Reset();
    }
    return SUCCEEDED(result);
}

bool DockWindowTransition::ApplyFrame(double progress)
{
    if (!hwnd_ ||
        surface_ ==
            DockWindowTransitionSurface::None)
        return false;

    const RECT frame = InterpolateDockWindowTransitionRect(
        fromRect_, toRect_, progress);
    const int width = std::max(1L, frame.right - frame.left);
    const int height =
        std::max(1L, frame.bottom - frame.top);
    const double eased =
        EaseDockWindowTransition(progress);
    const BYTE frameOpacity =
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

    if (surface_ ==
        DockWindowTransitionSurface::Snapshot)
    {
        if (opacityChanged &&
            !SetLayeredWindowAttributes(
                hwnd_, 0, frameOpacity,
                LWA_ALPHA))
            return false;
        if (geometryChanged)
        {
            RECT localFrame = frame;
            OffsetRect(
                &localFrame,
                -snapshotHostRect_.left,
                -snapshotHostRect_.top);
            HRGN visibleRegion =
                CreateDockWindowTransitionRegion(
                    localFrame, cornerRadius);
            if (!visibleRegion)
                return false;
            if (!SetWindowRgn(
                    hwnd_, visibleRegion, FALSE))
            {
                DeleteObject(visibleRegion);
                return false;
            }
            if (!DrawSnapshotFrame(localFrame))
                return false;
        }
    }
    else
    {
        if (!thumbnail_)
            return false;
        if (geometryChanged)
        {
            SetWindowPos(
                hwnd_, nullptr,
                frame.left, frame.top,
                width, height,
                SWP_NOACTIVATE |
                    SWP_NOOWNERZORDER |
                    SWP_NOZORDER |
                    SWP_NOSENDCHANGING |
                    SWP_NOCOPYBITS);
            const RECT localFrame{
                0, 0, width, height
            };
            HRGN visibleRegion =
                CreateDockWindowTransitionRegion(
                    localFrame, cornerRadius);
            if (!visibleRegion)
                return false;
            if (!SetWindowRgn(
                    hwnd_, visibleRegion, FALSE))
            {
                DeleteObject(visibleRegion);
                return false;
            }
        }
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags =
            DWM_TNP_RECTDESTINATION |
            DWM_TNP_VISIBLE |
            DWM_TNP_OPACITY |
            DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination =
            { 0, 0, width, height };
        properties.opacity = frameOpacity;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly =
            FALSE;
        if (FAILED(DwmUpdateThumbnailProperties(
                thumbnail_, &properties)))
            return false;
    }
    lastFrameRect_ = frame;
    lastFrameOpacity_ = frameOpacity;
    hasLastFrame_ = true;
    return true;
}

void DockWindowTransition::OnTimer()
{
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        Finish();
        return;
    }

    const double now =
        MonotonicTimeMilliseconds();
    if (awaitingRestoreVisibility_)
    {
        if (!IsIconic(sourceWindow_) ||
            now >= restoreCleanupDeadlineMs_)
            Finish();
        return;
    }

    const double progress = std::min(
        1.0,
        (now - animationStartTimeMs_) /
            std::max(1.0, animationDurationMs_));
    if (!ApplyFrame(progress))
    {
        CompleteRestoreAfterRenderFailure();
        Finish();
        return;
    }
    if (progress < 1.0)
        return;

    if (direction_ == DockWindowTransitionDirection::Restore &&
        restoreCallback_)
    {
        RestoreCallback callback =
            std::move(restoreCallback_);
        callback(sourceWindow_);
        awaitingRestoreVisibility_ = true;
        restoreCleanupDeadlineMs_ =
            now + static_cast<double>(
                kRestoreCleanupTimeoutMs);
        return;
    }
    Finish();
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
    callback(sourceWindow_);
}

void DockWindowTransition::SetNativeTransitionsDisabled(
    bool disabled)
{
    if (!sourceWindow_ || !IsWindow(sourceWindow_))
    {
        nativeTransitionsDisabled_ = false;
        return;
    }
    const BOOL value = disabled ? TRUE : FALSE;
    DwmSetWindowAttribute(
        sourceWindow_,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &value, sizeof(value));
    nativeTransitionsDisabled_ = disabled;
}

void DockWindowTransition::UnregisterThumbnail()
{
    if (thumbnail_)
        DwmUnregisterThumbnail(thumbnail_);
    thumbnail_ = nullptr;
}

void DockWindowTransition::Finish()
{
    HWND transitionWindow = hwnd_;
    if (transitionWindow)
    {
        KillTimer(
            transitionWindow,
            kAnimationTimerId);
        ShowWindow(
            transitionWindow, SW_HIDE);
        SetWindowRgn(
            transitionWindow, nullptr, FALSE);
        SetLayeredWindowAttributes(
            transitionWindow, 0, 255,
            LWA_ALPHA);
    }
    UnregisterThumbnail();
    activeSnapshotBitmap_.Reset();
    if (nativeTransitionsDisabled_)
    {
        // Keep native transitions disabled until DWM has committed the
        // real minimized/restored state. Re-enabling too early lets the
        // system animation trail the snapshot as a dark second window.
        DwmFlush();
        SetNativeTransitionsDisabled(false);
    }
    sourceWindow_ = nullptr;
    surface_ =
        DockWindowTransitionSurface::None;
    fromRect_ = {};
    toRect_ = {};
    windowRect_ = {};
    dockRect_ = {};
    snapshotHostRect_ = {};
    lastFrameRect_ = {};
    lastFrameOpacity_ = 0;
    hasLastFrame_ = false;
    animationStartTimeMs_ = 0.0;
    animationDurationMs_ =
        static_cast<double>(
            kAnimationDurationMs);
    restoreCleanupDeadlineMs_ = 0.0;
    animationFromOpacity_ = 255;
    animationToOpacity_ = 0;
    awaitingRestoreVisibility_ = false;
    restoreCallback_ = {};
}

void DockWindowTransition::Cancel()
{
    Finish();
}

bool DockWindowTransition::IsActive() const
{
    return sourceWindow_ != nullptr &&
        surface_ !=
            DockWindowTransitionSurface::None;
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
    return sourceWindow_ == root;
}

DockWindowTransitionDirection
DockWindowTransition::GetDirection() const
{
    return direction_;
}

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

    switch (message)
    {
    case WM_TIMER:
        if (self && wParam == kAnimationTimerId)
        {
            self->OnTimer();
            return 0;
        }
        break;
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
        if (self &&
            self->surface_ ==
                DockWindowTransitionSurface::Snapshot &&
            self->hasLastFrame_)
        {
            RECT localFrame =
                self->lastFrameRect_;
            OffsetRect(
                &localFrame,
                -self->snapshotHostRect_.left,
                -self->snapshotHostRect_.top);
            self->DrawSnapshotFrame(
                localFrame);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (self)
            self->hwnd_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(
        window, message, wParam, lParam);
}
