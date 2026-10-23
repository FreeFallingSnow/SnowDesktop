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

    ~Session()
    {
        if (request) UnmapViewOfFile(request);
        if (mapping) CloseHandle(mapping);
        if (owner) CloseHandle(owner);
    }

    void Restore() noexcept
    {
        try
        {
            if (frame && group && frame.RenderTransform() == group)
                frame.RenderTransform(original);
            frame = nullptr; visualRoot = nullptr; group = nullptr; translation = nullptr; original = nullptr;
        }
        catch (...) {}
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
        RECT bounds{};
        if (!GetWindowRect(window, &bounds)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
        xaml::FrameworkElement root{nullptr};
        try
        {
            const auto current = xaml::Window::Current();
            if (current) root = current.Content().try_as<xaml::FrameworkElement>();
        }
        catch (...) {} // Compact native hosts need no XAML projection.
        const bool retained = frame && root == visualRoot &&
            (frame == root || media::VisualTreeHelper::GetParent(frame));
        auto candidate = retained ? frame : (root ? FindFrame(root) : nullptr);
        if (!candidate)
        {
            // Windows 10's compact Start host can expose its actual menu as
            // the HWND bounds. Fullscreen/unknown XAML hosts are not guessed.
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &info))
                return Fail(ERROR_INVALID_MONITOR_HANDLE);
            const SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
            if (size.cx < 200 || size.cy < 200 ||
                size.cx >= info.rcMonitor.right - info.rcMonitor.left - 32 ||
                size.cy >= info.rcMonitor.bottom - info.rcMonitor.top - 32)
                return GetTickCount64() - started < 750 ? true : Fail(ERROR_NOT_SUPPORTED);
            const auto target = Position(placement, size);
            if (!target) return Fail(ERROR_INSUFFICIENT_BUFFER);
            if (!MoveWindowTo(*target)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
            InterlockedExchange(&request->status, Positioned);
            return true;
        }

        if (frame && candidate != frame)
        {
            if (group && frame.RenderTransform() == group) frame.RenderTransform(original);
            frame = nullptr; group = nullptr; translation = nullptr; original = nullptr;
        }
        if (candidate.ActualWidth() <= 0 || candidate.ActualHeight() <= 0) return true;

        // Rehost on the invoking monitor before measuring XAML, so its DPI and
        // responsive layout settle on that monitor. The following ticks verify
        // the actual resulting geometry, with a bounded startup deadline.
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &info))
            return Fail(ERROR_INVALID_MONITOR_HANDLE);
        if (!EqualRect(&info.rcMonitor, &placement.screen))
        {
            if (!MoveWindowTo(placement.screen)) return Fail(ERROR_NOT_SUPPORTED);
            return true;
        }

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
        }
        else if (frame.RenderTransform() != group)
            return Fail(ERROR_NOT_SUPPORTED); // Another Shell customizer owns it now.

        const double scale = static_cast<double>(GetDpiForWindow(window)) / 96.0;
        if (scale <= 0 || !std::isfinite(scale)) return Fail(ERROR_INVALID_DATA);
        POINT origin{};
        if (!ClientToScreen(window, &origin)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
        const auto measure = [&] {
            return frame.TransformToVisual(nullptr).TransformBounds({0, 0,
                static_cast<float>(frame.ActualWidth()), static_cast<float>(frame.ActualHeight())});
        };
        const auto rect = measure();
        const SIZE size{static_cast<LONG>(std::ceil(rect.Width * scale)),
            static_cast<LONG>(std::ceil(rect.Height * scale))};
        const auto target = Position(placement, size);
        if (!target) return Fail(ERROR_INSUFFICIENT_BUFFER);
        RECT client{};
        GetClientRect(window, &client);
        OffsetRect(&client, origin.x, origin.y);
        if (target->left < client.left || target->top < client.top ||
            target->right > client.right || target->bottom > client.bottom)
            return Fail(ERROR_NOT_SUPPORTED);
        translation.X(translation.X() + (target->left - origin.x) / scale - rect.X);
        translation.Y(translation.Y() + (target->top - origin.y) / scale - rect.Y);
        const auto placed = measure();
        if (std::abs(origin.x + placed.X * scale - target->left) > 3 ||
            std::abs(origin.y + placed.Y * scale - target->top) > 3)
            return GetTickCount64() - started < 1500 ? true : Fail(ERROR_NOT_SUPPORTED);
        InterlockedExchange(&request->status, Positioned);
        return true;
    }

    bool Tick()
    {
        if (moving) return true;
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
        if (!SetTimer(window, kTimer, 50, nullptr))
        {
            RemoveWindowSubclass(window, Subclass, kSubclass);
            session->Fail(ERROR_NOT_ENOUGH_MEMORY); return;
        }
        InterlockedExchange(&request.status, Prepared);
        session.release();
    }
    catch (...)
    {
        if (session && session->request) session->Fail(static_cast<DWORD>(winrt::to_hresult()));
    }
}
}
