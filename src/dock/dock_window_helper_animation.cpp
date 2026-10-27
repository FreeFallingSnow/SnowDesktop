#include "dock_window_helper_animation.h"
#include "dock_external_minimize.h"
#include "dock_minimize_protocol.h"
#include "dock_window_transition.h"
#include "platform/window_support/window_support_security.h"
#include "platform/window_support/window_support_pointer_metadata.h"
#include "settings/animation_settings.h"

#include <algorithm>

namespace support = snowdesktop::window_support;
namespace wire = support::protocol;
struct DockWindowHelperAnimation::Impl
{
    snowdesktop::UiAnimationScheduler scheduler;
    DockWindowTransition transition;
    DockExternalMinimize hooks;
    std::vector<wire::Target> targets;
    HWND receiver = nullptr;
    wire::WindowIdentity current, minimized;
    wire::WindowIdentity hostReceiver, nativeMinimizing;
    RECT nativeMinimizeFrame{};
    DWORD nativeMinimizeAt = 0;
    bool ready = false;
    ~Impl()
    {
        transition.CompleteImmediately();
        hooks.Stop();
        if (receiver) DestroyWindow(receiver);
    }
    static void Preferences(const wire::Target& target)
    {
        snowdesktop::animation::SetRuntimePreferences(target.animate ? 1 : 2, 2,
            static_cast<int>(target.speed), static_cast<int>(target.frameLimit), true, false,
            static_cast<int>(target.effect), static_cast<int>(target.dockPosition));
    }
    void UpdateHooks()
    {
        std::vector<HWND> windows;
        for (const auto& target : targets)
            if (wire::Current(target.identity)) windows.push_back(wire::Window(target.identity));
        std::sort(windows.begin(), windows.end(), std::less<HWND>{});
        windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
        hooks.UpdateTargets(windows);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == snowdesktop::dock_minimize::RequestMessage())
        {
            const HWND source = reinterpret_cast<HWND>(wp);
            const auto found = std::find_if(self->targets.begin(), self->targets.end(), [source](const auto& value) {
                return wire::Window(value.identity) == source && wire::Current(value.identity);
            });
            if (found == self->targets.end() || !self->hooks.OwnsTarget(source)) return FALSE;
            self->nativeMinimizing = found->identity;
            self->nativeMinimizeAt = GetTickCount();
            self->nativeMinimizeFrame = {};
            GetWindowRect(source, &self->nativeMinimizeFrame);
            // Establish the host's native-minimize protection before the CBT
            // caller can change foreground. No polling round trip is required.
            if (wire::Current(self->hostReceiver))
            {
                DWORD_PTR acknowledged = 0;
                SendMessageTimeoutW(wire::Window(self->hostReceiver), support::BeforeNativeMinimizeMessage(),
                    reinterpret_cast<WPARAM>(source), lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 12, &acknowledged);
            }
            Preferences(*found);
            self->transition.ObserveNativeMinimize(source);
            if (!found->animate || IsRectEmpty(&found->anchor)) return FALSE;
            const bool started = self->transition.StartExternalMinimize(source, found->anchor, static_cast<DWORD>(lp),
                reinterpret_cast<HWND>(static_cast<UINT_PTR>(found->keepBelow)));
            if (started) self->current = found->identity;
            return started;
        }
        if (self && message == snowdesktop::dock_minimize::CancelMessage())
        {
            self->transition.CancelExternalMinimize(reinterpret_cast<HWND>(wp), static_cast<DWORD>(lp));
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
DockWindowHelperAnimation::DockWindowHelperAnimation() : impl_(std::make_unique<Impl>()) {}
DockWindowHelperAnimation::~DockWindowHelperAnimation() = default;
bool DockWindowHelperAnimation::Initialize()
{
    if (!impl_->scheduler.Initialize()) return false;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> device;
    if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&device)))) return false;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!impl_->transition.Initialize(instance, &impl_->scheduler, nullptr, device.Get()) ||
        !impl_->transition.ReadyForAnimation()) return false;
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpszClassName = L"SnowDesktopWindowSupportReceiver";
    type.lpfnWndProc = &Impl::WindowProc;
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    impl_->receiver = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, impl_.get());
    const auto directory = support::HelperDirectory();
    if (!impl_->receiver || !impl_->hooks.Start(impl_->receiver,
        (directory / L"SnowDesktopDockMinimizeHook.dll").wstring(),
        (directory / L"SnowDesktopDockMinimizeHost32.exe").wstring(),
        (directory / L"SnowDesktopDockMinimizeHook32.dll").wstring())) return false;
    impl_->transition.SetMinimizeCompletionCallbacks([state = impl_.get()](HWND window, std::optional<RECT> anchor)
        -> std::optional<DockWindowTransition::MinimizeTarget> {
        for (const auto& target : state->targets)
            if (wire::Window(target.identity) == window && wire::Current(target.identity))
                return DockWindowTransition::MinimizeTarget{reinterpret_cast<HWND>(static_cast<UINT_PTR>(target.keepBelow)),
                    MonitorFromRect(&target.anchor, MONITOR_DEFAULTTONEAREST), anchor.value_or(target.anchor)};
        return {};
    }, [state = impl_.get()](const DockWindowTransition::MinimizeTarget&) { state->minimized = state->current; });
    impl_->ready = true;
    return true;
}
bool DockWindowHelperAnimation::Available() const noexcept { return impl_->ready; }
snowdesktop::UiAnimationScheduler& DockWindowHelperAnimation::Scheduler() { return impl_->scheduler; }
bool DockWindowHelperAnimation::Configure(const wire::Request& request)
{
    if (!Available()) return false;
    impl_->hostReceiver = wire::Current(request.target) && request.target.process == request.hostProcess
        ? request.target : wire::WindowIdentity{};
    std::vector<wire::Target> desired;
    for (std::size_t i = 0; i < request.targetCount; ++i)
        if (wire::Current(request.targets[i].identity) && support::TargetInCurrentSession(wire::Window(request.targets[i].identity)))
            desired.push_back(request.targets[i]);
    impl_->targets = std::move(desired);
    impl_->UpdateHooks();
    return true;
}
bool DockWindowHelperAnimation::Start(const wire::Target& target, bool restore)
{
    if (!Available() || !wire::Current(target.identity) || IsRectEmpty(&target.anchor) || !target.animate) return false;
    auto found = std::find_if(impl_->targets.begin(), impl_->targets.end(), [&](const auto& value) {
        return value.identity.window == target.identity.window;
    });
    if (found == impl_->targets.end()) impl_->targets.push_back(target);
    else *found = target;
    impl_->UpdateHooks();
    if (!impl_->hooks.OwnsTarget(wire::Window(target.identity))) return false;
    Impl::Preferences(target);
    impl_->current = target.identity;
    const HWND window = wire::Window(target.identity);
    const HWND below = reinterpret_cast<HWND>(static_cast<UINT_PTR>(target.keepBelow));
    if (!restore) return impl_->transition.StartMinimize(window, target.anchor, below, [](HWND source) {
        PostMessageW(source, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    });
    return impl_->transition.StartRestore(window, target.anchor, [](HWND source, DockWindowRestoreTransitionPhase phase) {
        if (phase == DockWindowRestoreTransitionPhase::RequestRestore || phase == DockWindowRestoreTransitionPhase::FallbackWithoutAnimation)
            ShowWindowAsync(source, SW_RESTORE);
        if (phase == DockWindowRestoreTransitionPhase::ActivateRestored) SetForegroundWindow(source);
    }, below);
}
void DockWindowHelperAnimation::Cancel(bool complete)
{ if (complete) impl_->transition.CompleteImmediately(); else impl_->transition.Cancel(); }
void DockWindowHelperAnimation::Snapshot(wire::Response& response)
{
    response.animationActive = impl_->transition.IsActive();
    response.animationTarget = impl_->current;
    response.direction = static_cast<std::uint32_t>(impl_->transition.GetDirection());
    response.presentation = reinterpret_cast<UINT_PTR>(impl_->transition.GetPresentationWindow());
    response.externalMinimize = std::exchange(impl_->minimized, {});
}
void DockWindowHelperAnimation::AnnotatePointerEvents(wire::Response& response)
{
    for (std::size_t i = 0; i < response.pointerCount; ++i)
    {
        auto& event = response.pointerEvents[i];
        if (event.message != WM_LBUTTONDOWN) continue;
        const HWND target = GetAncestor(WindowFromPoint(event.point), GA_ROOT);
        const bool caption = impl_->hooks.IsMinimizeButtonPress(target, event.point);
        const bool observed = wire::Current(impl_->nativeMinimizing) &&
            support::PressPrecedesObservedMinimize(event, impl_->nativeMinimizeFrame, impl_->nativeMinimizeAt, GetTickCount());
        if (caption || observed) event.flags |= support::PointerMinimizeIntent;
    }
}
