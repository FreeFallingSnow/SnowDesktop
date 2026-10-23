#include "start_menu_protocol.h"

#include <commctrl.h>
#include <dwmapi.h>
#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#pragma pop_macro("GetCurrentTime")
#include <cmath>
#include <memory>
#include <vector>

namespace snowdesktop::start_menu
{
namespace
{
namespace xaml = winrt::Windows::UI::Xaml;
namespace media = winrt::Windows::UI::Xaml::Media;
constexpr UINT_PTR kSubclass = 0x5344534d;
constexpr UINT_PTR kTimer = 0x5344534e;
LRESULT CALLBACK Subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

struct Session
{
    HWND window = nullptr;
    HANDLE mapping = nullptr, owner = nullptr;
    Request* request = nullptr;
    Placement placement;
    ULONGLONG started = GetTickCount64();
    bool visibleSeen = false, moving = false, moved = false;
    unsigned hiddenSamples = 0;
    unsigned relocations = 0;
    RECT originalWindow{}, appliedWindow{};
    xaml::FrameworkElement frame{nullptr};
    xaml::FrameworkElement visualRoot{nullptr};
    media::Transform original{nullptr};
    media::TransformGroup group{nullptr};
    media::TranslateTransform translation{nullptr};
    bool layoutOwned = false, ticking = false;
    xaml::HorizontalAlignment originalHorizontal{}, appliedHorizontal{};
    xaml::VerticalAlignment originalVertical{}, appliedVertical{};
    xaml::Thickness originalMargin{}, appliedMargin{};

    ~Session()
    {
        if (request) UnmapViewOfFile(request);
        if (mapping) CloseHandle(mapping);
        if (owner) CloseHandle(owner);
    }

    static bool SameMargin(const xaml::Thickness& a, const xaml::Thickness& b) noexcept
    {
        return a.Left == b.Left && a.Top == b.Top && a.Right == b.Right && a.Bottom == b.Bottom;
    }

    void RestoreFrame() noexcept
    {
        try
        {
            if (frame && group && frame.RenderTransform() == group)
                frame.RenderTransform(original);
            if (frame && layoutOwned)
            {
                if (frame.HorizontalAlignment() == appliedHorizontal) frame.HorizontalAlignment(originalHorizontal);
                if (frame.VerticalAlignment() == appliedVertical) frame.VerticalAlignment(originalVertical);
                if (SameMargin(frame.Margin(), appliedMargin)) frame.Margin(originalMargin);
            }
        }
        catch (...) {}
        layoutOwned = false;
        frame = nullptr; visualRoot = nullptr; group = nullptr; translation = nullptr; original = nullptr;
    }

    void Restore() noexcept
    {
        PublishBounds(*request, RECT{});
        RestoreFrame();
        if (moved && IsWindow(window))
        {
            RECT current{};
            // Never overwrite a subsequent Shell relocation or a replacement
            // window. Restore only geometry that this session still owns.
            if (GetWindowRect(window, &current) && EqualRect(&current, &appliedWindow))
                SetWindowPos(window, nullptr, originalWindow.left, originalWindow.top,
                    originalWindow.right - originalWindow.left, originalWindow.bottom - originalWindow.top,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        moved = false;
    }

    bool PrepareRedesignedLayout(double scale)
    {
        // The redesigned menu participates in a Grid's layout. Moving only its
        // render transform leaves the original layout allocation/clipping in
        // place. Reserve the inward area during layout, then measure again on
        // the next pulse. See Windhawk's shell-flyout-positions capability path:
        // https://github.com/ramensoftware/windhawk-mods/blob/main/mods/shell-flyout-positions.wh.cpp
        const auto area = AvailableArea(placement);
        if (!area) return false;
        originalHorizontal = frame.HorizontalAlignment();
        originalVertical = frame.VerticalAlignment();
        originalMargin = frame.Margin();
        appliedHorizontal = placement.edge == Edge::Left ? xaml::HorizontalAlignment::Left :
            placement.edge == Edge::Right ? xaml::HorizontalAlignment::Right : xaml::HorizontalAlignment::Center;
        appliedVertical = placement.edge == Edge::Top ? xaml::VerticalAlignment::Top :
            placement.edge == Edge::Bottom ? xaml::VerticalAlignment::Bottom : xaml::VerticalAlignment::Center;
        appliedMargin = {
            std::max(0.0, (area->left - placement.screen.left) / scale),
            std::max(0.0, (area->top - placement.screen.top) / scale),
            std::max(0.0, (placement.screen.right - area->right) / scale),
            std::max(0.0, (placement.screen.bottom - area->bottom) / scale)};
        if (appliedHorizontal == xaml::HorizontalAlignment::Center)
            appliedMargin.Left = appliedMargin.Right = std::max({appliedMargin.Left,
                appliedMargin.Right, originalMargin.Left, originalMargin.Right});
        if (appliedVertical == xaml::VerticalAlignment::Center)
            appliedMargin.Top = appliedMargin.Bottom = std::max({appliedMargin.Top,
                appliedMargin.Bottom, originalMargin.Top, originalMargin.Bottom});
        // Retain native breathing room on the opposite edge. The near edge is
        // owned by the Dock gap, not the Windows taskbar's original spacing.
        if (placement.edge == Edge::Bottom) appliedMargin.Top = std::max(appliedMargin.Top, originalMargin.Top);
        if (placement.edge == Edge::Top) appliedMargin.Bottom = std::max(appliedMargin.Bottom, originalMargin.Bottom);
        if (placement.edge == Edge::Left) appliedMargin.Right = std::max(appliedMargin.Right, originalMargin.Right);
        if (placement.edge == Edge::Right) appliedMargin.Left = std::max(appliedMargin.Left, originalMargin.Left);
        layoutOwned = true;
        frame.HorizontalAlignment(appliedHorizontal);
        frame.VerticalAlignment(appliedVertical);
        frame.Margin(appliedMargin);
        return true;
    }

    bool Fail(DWORD error)
    {
        Restore();
        request->error = static_cast<LONG>(error);
        InterlockedExchange(&request->status, Unsupported);
        return false;
    }

    xaml::FrameworkElement FindFrame(const xaml::FrameworkElement& root)
    {
        // Capability detection, not an OS build-number whitelist. Unknown
        // layouts remain native instead of modifying arbitrary Shell visuals.
        const auto rootClass = winrt::get_class_name(root);
        const bool redesigned = rootClass == L"StartMenu.StartBlendedFlexFrame";
        std::vector<xaml::DependencyObject> pending{root};
        unsigned visited = 0;
        while (!pending.empty() && ++visited <= 256)
        {
            auto node = pending.back(); pending.pop_back();
            auto element = node.try_as<xaml::FrameworkElement>();
            if (element && (winrt::get_class_name(element) == L"StartDocked.StartSizingFrame" ||
                (redesigned && element.Name() == L"FrameRoot"))) return element;
            const int count = media::VisualTreeHelper::GetChildrenCount(node);
            for (int i = count - 1; i >= 0 && pending.size() < 256; --i)
                pending.push_back(media::VisualTreeHelper::GetChild(node, i));
        }
        return nullptr;
    }

    bool MoveWindowTo(const RECT& target)
    {
        RECT current{};
        if (!GetWindowRect(window, &current)) return false;
        if (EqualRect(&current, &target)) return true;
        if (++relocations > 8) return false; // Do not fight a Shell reflow forever.
        if (!moved) originalWindow = current;
        moving = true;
        const bool ok = SetWindowPos(window, nullptr, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER) != FALSE;
        moving = false;
        if (!ok || !GetWindowRect(window, &appliedWindow)) return false;
        moved = true;
        return EqualRect(&appliedWindow, &target) != FALSE;
    }

    bool Apply()
    {
        InterlockedExchange(&request->stage, ReadingRoot);
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
        xaml::FrameworkElement root{nullptr};
        try
        {
            const auto current = xaml::Window::Current();
            if (current) root = current.Content().try_as<xaml::FrameworkElement>();
        }
        catch (...) {} // Compact native hosts need no XAML projection.
        if (root) wcsncpy_s(request->rootClass, winrt::get_class_name(root).c_str(), _TRUNCATE);
        const bool retained = frame && root == visualRoot &&
            (frame == root || media::VisualTreeHelper::GetParent(frame));
        auto candidate = retained ? frame : (root ? FindFrame(root) : nullptr);
        if (candidate) wcsncpy_s(request->frameClass, winrt::get_class_name(candidate).c_str(), _TRUNCATE);
        if (!candidate)
        {
            // Windows 10's compact Start host can expose its actual menu as
            // the HWND bounds. Fullscreen/unknown XAML hosts are not guessed.
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &info))
                return Fail(ERROR_INVALID_MONITOR_HANDLE);
            const SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
            request->measuredSize = size;
            if (size.cx < 200 || size.cy < 200 ||
                size.cx >= info.rcMonitor.right - info.rcMonitor.left - 32 ||
                size.cy >= info.rcMonitor.bottom - info.rcMonitor.top - 32)
                return GetTickCount64() - started < 750 ? true : Fail(ERROR_NOT_SUPPORTED);
            const auto target = Position(placement, size);
            if (!target) return Fail(ERROR_INSUFFICIENT_BUFFER);
            InterlockedExchange(&request->stage, MovingHost);
            if (!MoveWindowTo(*target)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
            PublishBounds(*request, *target);
            InterlockedExchange(&request->stage, PlacementApplied);
            InterlockedExchange(&request->status, Positioned);
            return true;
        }

        if (frame && candidate != frame)
            RestoreFrame();
        if (candidate.ActualWidth() <= 0 || candidate.ActualHeight() <= 0)
        {
            InterlockedExchange(&request->stage, WaitingForLayout);
            return true;
        }

        // Rehost on the invoking monitor before measuring XAML, so its DPI and
        // responsive layout settle on that monitor. The following ticks verify
        // the actual resulting geometry, with a bounded startup deadline.
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &info))
            return Fail(ERROR_INVALID_MONITOR_HANDLE);
        if (!EqualRect(&info.rcMonitor, &placement.screen))
        {
            InterlockedExchange(&request->stage, MovingHost);
            if (!MoveWindowTo(placement.screen)) return Fail(ERROR_NOT_SUPPORTED);
            return true;
        }

        const double scale = static_cast<double>(GetDpiForWindow(window)) / 96.0;
        if (scale <= 0 || !std::isfinite(scale)) return Fail(ERROR_INVALID_DATA);
        if (!frame)
        {
            frame = candidate;
            visualRoot = root;
            original = frame.RenderTransform();
            group = media::TransformGroup{};
            translation = media::TranslateTransform{};
            if (original) group.Children().Append(original);
            group.Children().Append(translation);
            frame.RenderTransform(group);
            if (winrt::get_class_name(root) == L"StartMenu.StartBlendedFlexFrame")
            {
                if (!PrepareRedesignedLayout(scale)) return Fail(ERROR_NOT_SUPPORTED);
                InterlockedExchange(&request->stage, WaitingForLayout);
                return true;
            }
        }
        else if (frame.RenderTransform() != group)
            return Fail(ERROR_NOT_SUPPORTED); // Another Shell customizer owns it now.
        if (layoutOwned && (frame.HorizontalAlignment() != appliedHorizontal ||
            frame.VerticalAlignment() != appliedVertical || !SameMargin(frame.Margin(), appliedMargin)))
            return Fail(ERROR_NOT_SUPPORTED); // Do not fight a concurrent layout owner.

        POINT origin{};
        if (!ClientToScreen(window, &origin)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
        const auto measure = [&] {
            return frame.TransformToVisual(nullptr).TransformBounds({0, 0,
                static_cast<float>(frame.ActualWidth()), static_cast<float>(frame.ActualHeight())});
        };
        const auto rect = measure();
        const SIZE size{static_cast<LONG>(std::ceil(rect.Width * scale)),
            static_cast<LONG>(std::ceil(rect.Height * scale))};
        request->measuredSize = size;
        const auto target = Position(placement, size);
        if (!target) return Fail(ERROR_INSUFFICIENT_BUFFER);
        RECT client{};
        GetClientRect(window, &client);
        OffsetRect(&client, origin.x, origin.y);
        if (target->left < client.left || target->top < client.top ||
            target->right > client.right || target->bottom > client.bottom)
            return Fail(ERROR_NOT_SUPPORTED);
        InterlockedExchange(&request->stage, MovingFrame);
        translation.X(translation.X() + (target->left - origin.x) / scale - rect.X);
        translation.Y(translation.Y() + (target->top - origin.y) / scale - rect.Y);
        const auto placed = measure();
        if (std::abs(origin.x + placed.X * scale - target->left) > 3 ||
            std::abs(origin.y + placed.Y * scale - target->top) > 3)
            return GetTickCount64() - started < 1500 ? true : Fail(ERROR_NOT_SUPPORTED);
        PublishBounds(*request, *target);
        InterlockedExchange(&request->stage, PlacementApplied);
        InterlockedExchange(&request->status, Positioned);
        return true;
    }

    bool Tick()
    {
        if (moving || ticking) return true;
        struct TickGuard
        {
            bool& flag;
            explicit TickGuard(bool& value) : flag(value) { flag = true; }
            ~TickGuard() { flag = false; }
        } guard(ticking);
        InterlockedIncrement(&request->ticks);
        const ULONGLONG now = GetTickCount64();
        const auto beat = static_cast<ULONGLONG>(InterlockedCompareExchange64(&request->heartbeat, 0, 0));
        if (!request->active || beat > now || now - beat > 3000 ||
            WaitForSingleObject(owner, 0) != WAIT_TIMEOUT)
        {
            Restore();
            InterlockedExchange(&request->status, Closed);
            return false;
        }
        DWORD cloak = 0;
        const bool visible = IsWindowVisible(window) &&
            SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloak, sizeof(cloak))) && !cloak;
        if (visible) { visibleSeen = true; hiddenSamples = 0; }
        else if (visibleSeen && ++hiddenSamples >= 2)
        {
            Restore();
            InterlockedExchange(&request->status, Closed);
            return false;
        }
        if (!visible && !visibleSeen && now - started >= 2000) return Fail(ERROR_TIMEOUT);
        if (now - started >= 2000 && request->status != Positioned) return Fail(ERROR_TIMEOUT);
        return Apply();
    }
};

void Finish(Session* session)
{
    KillTimer(session->window, kTimer);
    RemoveWindowSubclass(session->window, Subclass, kSubclass);
    delete session;
}

LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR value)
{
    auto* session = reinterpret_cast<Session*>(value);
    if (message == WM_NCDESTROY)
    {
        session->Restore();
        InterlockedExchange(&session->request->status, Closed);
        Finish(session);
        return DefSubclassProc(window, message, wp, lp);
    }
    const bool tick = (message == WM_TIMER && wp == kTimer) ||
        message == RegisterWindowMessageW(kMessage);
    if (tick)
    {
        bool keep = false;
        try { keep = session->Tick(); }
        catch (...) { session->Fail(static_cast<DWORD>(winrt::to_hresult())); }
        if (!keep) Finish(session);
        return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}
}

void Receive(HWND window, DWORD owner, std::uint64_t token) noexcept
{
    if (!owner || !token) return;
    std::unique_ptr<Session> session;
    try
    {
        session = std::make_unique<Session>();
        session->window = window;
        session->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MappingName(owner, token).c_str());
        if (!session->mapping) return;
        session->request = static_cast<Request*>(MapViewOfFile(session->mapping,
            FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)));
        if (!session->request) return;
        auto& request = *session->request;
        DWORD windowProcess = 0, ownerWindowProcess = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &windowProcess);
        GetWindowThreadProcessId(reinterpret_cast<HWND>(request.ownerWindow), &ownerWindowProcess);
        if (!ValidRequest(request, owner, GetCurrentProcessId(), token) || ownerWindowProcess != owner ||
            windowProcess != GetCurrentProcessId() || thread != GetCurrentThreadId()) return;
        session->owner = OpenProcess(SYNCHRONIZE, FALSE, owner);
        if (!session->owner || WaitForSingleObject(session->owner, 0) != WAIT_TIMEOUT)
        { session->Fail(ERROR_PROCESS_ABORTED); return; }
        DWORD_PTR previous = 0;
        if (GetWindowSubclass(window, Subclass, kSubclass, &previous))
        { session->Fail(ERROR_BUSY); return; }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&Subclass), &pinned))
        { session->Fail(GetLastError()); return; }
        session->placement = request.placement;
        if (!SetWindowSubclass(window, Subclass, kSubclass, reinterpret_cast<DWORD_PTR>(session.get())))
        { session->Fail(ERROR_NOT_SUPPORTED); return; }
        // The host drives placement with bounded messages. This slower local
        // timer is retained for cleanup if the owner disappears unexpectedly.
        if (!SetTimer(window, kTimer, 250, nullptr))
        {
            RemoveWindowSubclass(window, Subclass, kSubclass);
            session->Fail(ERROR_NOT_ENOUGH_MEMORY); return;
        }
        InterlockedExchange(&request.stage, Attached);
        InterlockedExchange(&request.status, Prepared);
        session.release();
    }
    catch (...)
    {
        if (session && session->request) session->Fail(static_cast<DWORD>(winrt::to_hresult()));
    }
}
}
