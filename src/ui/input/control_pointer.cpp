#include "control_pointer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace snowdesktop::controlled_pointer
{
namespace
{
constexpr ULONG_PTR InputTag = static_cast<ULONG_PTR>(0x534e4f5750545231ULL);
constexpr wchar_t WindowClass[] = L"SnowDesktop.ControlledPointer.v1";
double Now()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
RECT VirtualScreen()
{
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
POINT Center(RECT rect) { return {rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2}; }
bool Send(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0)
{
    INPUT input{}; input.type = INPUT_MOUSE;
    input.mi.dx = x; input.mi.dy = y; input.mi.mouseData = data;
    input.mi.dwFlags = flags; input.mi.dwExtraInfo = InputTag;
    return SendInput(1, &input, sizeof(input)) == 1;
}
void Escape()
{
    INPUT inputs[2]{};
    for (auto& input : inputs) { input.type = INPUT_KEYBOARD; input.ki.wVk = VK_ESCAPE; input.ki.dwExtraInfo = InputTag; }
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    (void)SendInput(2, inputs, sizeof(INPUT));
}
}

POINT Interpolate(POINT from, POINT to, double progress, double curvature)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    const double eased = t * t * (3.0 - 2.0 * t);
    const double dx = static_cast<double>(to.x) - from.x, dy = static_cast<double>(to.y) - from.y;
    const double arc = 4.0 * eased * (1.0 - eased) * std::clamp(curvature, -0.4, 0.4);
    return {static_cast<LONG>(std::lround(from.x + dx * eased - dy * arc)),
        static_cast<LONG>(std::lround(from.y + dy * eased + dx * arc))};
}
bool AbsolutePoint(POINT point, RECT screen, LONG& x, LONG& y)
{
    const auto width = screen.right - screen.left, height = screen.bottom - screen.top;
    if (width < 2 || height < 2 || !PtInRect(&screen, point)) return false;
    x = static_cast<LONG>(std::lround((static_cast<double>(point.x) - screen.left) * 65535.0 / (width - 1)));
    y = static_cast<LONG>(std::lround((static_cast<double>(point.y) - screen.top) * 65535.0 / (height - 1)));
    return true;
}

struct Player::Impl
{
    enum class Stage { Move, Pressed, Dragging, Released, SecondPressed };
    inline static thread_local Impl* active = nullptr;
    HWND window = nullptr;
    HHOOK mouseHook = nullptr, keyboardHook = nullptr;
    State state;
    Resolve resolve;
    Observe observe;
    RECT screen{};
    POINT start{}, anchor{};
    double started = 0, stageStarted = 0;
    Stage stage = Stage::Move;
    bool interrupted = false, ticking = false, observerFailed = false;

    bool Running() const { return state.status == "running"; }
    void Notify() noexcept { if (observe) try { observe(state); } catch (...) { observerFailed = true; } }
    void SamplePoint(POINT point, const char* phase)
    {
        state.current.point = point;
        state.current.milliseconds = std::max(0.0, Now() - started);
        state.current.phase = phase;
        state.trace.push_back(state.current);
        if (state.trace.size() > 4096) state.trace.erase(state.trace.begin());
        Notify();
        if (observerFailed) throw std::runtime_error("pointer_observer_failed");
    }
    void Release() noexcept
    {
        if (state.current.buttons && state.plan.action == Action::Drag) Escape();
        if (state.current.buttons & 1u) (void)Send(MOUSEEVENTF_LEFTUP);
        if (state.current.buttons & 2u) (void)Send(MOUSEEVENTF_RIGHTUP);
        state.current.buttons = 0;
    }
    void Finish(std::string status, std::string error = {}) noexcept
    {
        if (window) KillTimer(window, 1);
        if (mouseHook) { UnhookWindowsHookEx(mouseHook); mouseHook = nullptr; }
        if (keyboardHook) { UnhookWindowsHookEx(keyboardHook); keyboardHook = nullptr; }
        if (active == this) active = nullptr;
        Release();
        state.status = std::move(status); state.error = std::move(error);
        state.current.phase = state.status;
        state.current.milliseconds = std::max(0.0, Now() - started);
        Notify();
    }
    static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
    {
        if (code >= 0 && active && reinterpret_cast<MSLLHOOKSTRUCT*>(lp)->dwExtraInfo != InputTag)
        {
            const auto& event = *reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            if (!active->state.interruption)
                active->state.interruption = Interruption{"mouse", static_cast<UINT>(wp), event.flags, 0,
                    static_cast<std::uint64_t>(event.dwExtraInfo), static_cast<std::uint64_t>(InputTag), event.pt,
                    (event.flags & LLMHF_INJECTED) != 0, (event.flags & LLMHF_LOWER_IL_INJECTED) != 0};
            active->interrupted = true;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp)
    {
        if (code >= 0 && active && reinterpret_cast<KBDLLHOOKSTRUCT*>(lp)->dwExtraInfo != InputTag)
        {
            const auto& event = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            if (!active->state.interruption)
                active->state.interruption = Interruption{"keyboard", static_cast<UINT>(wp), event.flags, event.vkCode,
                    static_cast<std::uint64_t>(event.dwExtraInfo), static_cast<std::uint64_t>(InputTag), active->state.current.point,
                    (event.flags & LLKHF_INJECTED) != 0, (event.flags & LLKHF_LOWER_IL_INJECTED) != 0};
            active->interrupted = true;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_TIMER && wp == 1) { self->Tick(); return 0; }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    Target TargetFor(const std::string& id)
    {
        const auto target = resolve(id);
        if (IsRectEmpty(&target.bounds) || !IsWindow(target.window)) throw std::runtime_error("target_unavailable");
        return target;
    }
    void VerifyAt(const Target& target, POINT point)
    {
        DWORD expected = 0, actual = 0;
        GetWindowThreadProcessId(target.window, &expected);
        const auto hit = WindowFromPoint(point);
        if (hit) GetWindowThreadProcessId(hit, &actual);
        if (!expected || expected != actual || (hit != target.window && !IsChild(target.window, hit)) || !PtInRect(&target.bounds, point))
            throw std::runtime_error("target_occluded");
    }
    void Move(POINT point, const char* phase)
    {
        const auto currentScreen = VirtualScreen();
        if (!EqualRect(&currentScreen, &screen)) throw std::runtime_error("display_changed");
        LONG x = 0, y = 0;
        if (!AbsolutePoint(point, screen, x, y)) throw std::runtime_error("point_outside_desktop");
        if (!Send(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, x, y))
            throw std::runtime_error("input_rejected");
        SamplePoint(point, phase);
    }
    void Down(bool right)
    {
        if (!Send(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN)) throw std::runtime_error("input_rejected");
        state.current.buttons = right ? 2u : 1u;
        SamplePoint(state.current.point, "pressed");
    }
    void Up()
    {
        if (!Send(state.current.buttons == 2u ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP))
            throw std::runtime_error("input_rejected");
        state.current.buttons = 0;
        SamplePoint(state.current.point, "released");
    }
    void Tick() noexcept
    {
        if (!Running() || ticking) return;
        ticking = true;
        try
        {
            if (interrupted) { Finish("cancelled", "interrupted_by_user"); ticking = false; return; }
            const double now = Now(), elapsed = now - stageStarted;
            if (stage == Stage::Move)
            {
                const auto target = TargetFor(state.plan.sourceId);
                const auto end = Center(target.bounds);
                const double duration = state.plan.action == Action::Drag ? 250.0 : state.plan.durationMilliseconds;
                Move(Interpolate(start, end, elapsed / duration), "moving");
                if (elapsed >= duration)
                {
                    Move(end, "arrived"); VerifyAt(target, end); anchor = end;
                    if (state.plan.action == Action::Hover) Finish("completed");
                    else if (state.plan.action == Action::Scroll)
                    {
                        if (!Send(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(state.plan.wheelDelta)))
                            throw std::runtime_error("input_rejected");
                        SamplePoint(end, "scroll"); Finish("completed");
                    }
                    else { Down(state.plan.action == Action::ContextMenu); stage = Stage::Pressed; stageStarted = now; }
                }
            }
            else if (stage == Stage::Pressed && elapsed >= 80)
            {
                if (state.plan.action == Action::Drag) { stage = Stage::Dragging; stageStarted = now; }
                else { Up(); stage = Stage::Released; stageStarted = now; }
            }
            else if (stage == Stage::Dragging)
            {
                const auto target = TargetFor(state.plan.destinationId);
                const auto end = Center(target.bounds);
                Move(Interpolate(anchor, end, elapsed / state.plan.durationMilliseconds, 0.04), "dragging");
                if (elapsed >= state.plan.durationMilliseconds)
                {
                    Move(end, "drop"); VerifyAt(target, end); Up(); stage = Stage::Released; stageStarted = now;
                }
            }
            else if (stage == Stage::Released && elapsed >= 100)
            {
                if (state.plan.action == Action::DoubleClick)
                {
                    const auto target = TargetFor(state.plan.sourceId);
                    VerifyAt(target, state.current.point); Down(false); stage = Stage::SecondPressed; stageStarted = now;
                }
                else Finish("completed");
            }
            else if (stage == Stage::SecondPressed && elapsed >= 60) { Up(); Finish("completed"); }
        }
        catch (const std::exception& error) { Finish("failed", error.what()); }
        catch (...) { Finish("failed", "pointer_failure"); }
        ticking = false;
    }
};

Player::Player() : impl_(std::make_unique<Impl>()) {}
Player::~Player() { Stop(); }
const State& Player::Current() const { return impl_->state; }
bool Player::Start(Plan plan, Resolve resolve, Observe observe, std::string& error)
{
    error.clear();
    if (impl_->Running() || Impl::active) { error = "pointer_busy"; return false; }
    if (!resolve || plan.sourceId.empty() || !std::isfinite(plan.durationMilliseconds) ||
        plan.durationMilliseconds < 100 || plan.durationMilliseconds > 10000 ||
        (plan.action == Action::Drag && plan.destinationId.empty()) ||
        (plan.action == Action::Scroll && (!plan.wheelDelta || plan.wheelDelta < -12000 || plan.wheelDelta > 12000)))
    { error = "invalid_pointer_plan"; return false; }
    for (const int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) { error = "physical_input_active"; return false; }
    if (!impl_->window)
    {
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Impl::WindowProc; wc.lpszClassName = WindowClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { error = "pointer_window_failed"; return false; }
        impl_->window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, impl_.get());
        if (!impl_->window) { error = "pointer_window_failed"; return false; }
    }
    if (!GetCursorPos(&impl_->start)) { error = "cursor_unavailable"; return false; }
    impl_->resolve = std::move(resolve); impl_->observe = std::move(observe);
    const auto operation = impl_->state.operation + 1;
    impl_->state = {}; impl_->state.operation = operation; impl_->state.plan = std::move(plan);
    impl_->state.status = "running"; impl_->state.current.point = impl_->start;
    impl_->screen = VirtualScreen(); impl_->interrupted = false; impl_->stage = Impl::Stage::Move;
    impl_->observerFailed = false;
    impl_->started = impl_->stageStarted = Now();
    try { (void)impl_->TargetFor(impl_->state.plan.sourceId); }
    catch (const std::exception& failure) { impl_->Finish("failed", failure.what()); error = failure.what(); return false; }
    Impl::active = impl_.get();
    impl_->mouseHook = SetWindowsHookExW(WH_MOUSE_LL, Impl::MouseHook, GetModuleHandleW(nullptr), 0);
    impl_->keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, Impl::KeyboardHook, GetModuleHandleW(nullptr), 0);
    if (!impl_->mouseHook || !impl_->keyboardHook || !SetTimer(impl_->window, 1, 8, nullptr))
    { impl_->Finish("failed", "pointer_observer_unavailable"); error = impl_->state.error; return false; }
    impl_->Notify();
    if (impl_->observerFailed) { impl_->Finish("failed", "pointer_observer_failed"); error = impl_->state.error; return false; }
    return true;
}
void Player::Cancel(std::string reason) { if (impl_->Running()) impl_->Finish("cancelled", std::move(reason)); }
void Player::Stop() noexcept
{
    if (impl_->Running()) impl_->Finish("cancelled", "host_shutdown");
    if (impl_->window) { DestroyWindow(impl_->window); impl_->window = nullptr; }
    impl_->resolve = {}; impl_->observe = {};
}
}
