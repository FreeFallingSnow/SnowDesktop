#include "app/app.h"
#include "app_control_runtime.h"
#include "ui/menu/menu_control_snapshot.h"
#include "ui/input/text_input_window.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
using namespace snowdesktop::control;
constexpr UINT_PTR PrivatePointerCursorTimer = 0x53445043U;
bool Flag(const Value& value, const char* key, bool fallback)
{
    const auto* field = value.Find(key);
    if (!field) return fallback;
    if (!field->IsBoolean()) throw Error("invalid_params", std::string(key) + " must be boolean", -32602);
    return field->boolean;
}
bool Contains(HWND window, POINT point)
{
    RECT rect{};
    if (!window || !IsWindowVisible(window) || !GetWindowRect(window, &rect) || !PtInRect(&rect, point)) return false;
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return false;
    const int kind = GetWindowRgn(window, region);
    const bool contains = kind == ERROR || PtInRegion(region, point.x - rect.left, point.y - rect.top);
    DeleteObject(region);
    return contains;
}
std::string SessionId()
{
    GUID id{}; wchar_t text[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40))
        throw Error("input_unavailable", "Could not allocate a private input session");
    return WideToUtf8(text);
}
}

bool DesktopApp::OwnsPrivatePointerWindow(HWND window) const
{
    if (!window || !IsWindow(window)) return false;
    DWORD process = 0;
    if (GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return false;
    if (window == hwnd_ || window == inputHwnd_ || window == floatingDockInputHwnd_ ||
        window == floatingPopupHwnd_ || window == quickNavigationHwnd_) return true;
    // The desktop host is a child of Explorer's WorkerW. Windows promotes a
    // popup's native owner to that top-level window, so parent recursion is
    // not an ownership proof for this exact, application-created editor.
    if (window == renameInputWindow_)
        return snowdesktop::text_input::IsPrivatePointerHost(window);
    for (const auto& host : persistentDockHosts_) if (host && host->hwnd == window) return true;
    for (const auto& control : snowdesktop::modern_menu::ActiveControls()) if (control.window == window) return true;
    if (window == snowdesktop::modern_menu::ActiveCompanionWindow()) return true;
    if (snowdesktop::text_input::IsPrivatePointerHost(window))
    {
        const HWND parent = GetParent(window);
        return parent && parent != window && OwnsPrivatePointerWindow(parent);
    }
    return false;
}

HWND DesktopApp::PrivatePointerWindowAt(POINT point) const
{
    // This inventory is restricted to the application's actual native hosts.
    // The OS cursor, external windows and the decorative cursor overlay never
    // participate in private-device routing.
    const auto menus = snowdesktop::modern_menu::ActiveControls();
    const HWND companion = snowdesktop::modern_menu::ActiveCompanionWindow();
    if (OwnsPrivatePointerWindow(companion) && Contains(companion, point)) return companion;
    HWND modal = nullptr, hit = nullptr;
    int modalDepth = (std::numeric_limits<int>::max)(), hitDepth = -1;
    for (const auto& control : menus)
    {
        if (!control.pane || !OwnsPrivatePointerWindow(control.window) || !IsWindowVisible(control.window)) continue;
        if (control.depth < modalDepth) { modal = control.window; modalDepth = control.depth; }
        if (control.depth > hitDepth && Contains(control.window, point)) { hit = control.window; hitDepth = control.depth; }
    }
    if (modal) return hit ? hit : modal;
    for (const HWND window : {renameInputWindow_, quickNavigationHwnd_, floatingPopupHwnd_})
        if (Contains(window, point)) return window;
    for (auto it = persistentDockHosts_.rbegin(); it != persistentDockHosts_.rend(); ++it)
        if (*it && Contains((*it)->hwnd, point)) return (*it)->hwnd;
    return Contains(hwnd_, point) ? hwnd_ : nullptr;
}

void DesktopApp::PresentPrivatePointer()
{
    const auto& state = applicationControlRuntime_->privatePointer.Current();
    if (!state.enabled || state.closing || !applicationControlRuntime_->privatePointerShowCursor)
    {
        auto& runtime = *applicationControlRuntime_;
        if (runtime.privatePointerTimerHost && runtime.privatePointerTimer)
            KillTimer(runtime.privatePointerTimerHost, runtime.privatePointerTimer);
        runtime.privatePointerTimer = 0; runtime.privatePointerTimerHost = nullptr;
        runtime.cursor.Hide(); return;
    }
    snowdesktop::demo_cursor::Pose pose;
    pose.x = static_cast<float>(state.point.x); pose.y = static_cast<float>(state.point.y);
    pose.pressed = state.buttons != 0;
    const double now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - applicationControlRuntime_->privatePointerStarted).count();
    for (const auto& entry : state.trace)
        if (now - entry.milliseconds <= 180) pose.trail.push_back(entry.point);
    for (auto it = state.trace.rbegin(); it != state.trace.rend(); ++it)
        if (it->phase == "released") { pose.clickAgeMilliseconds = static_cast<float>(now - it->milliseconds); break; }
    if (!applicationControlRuntime_->cursor.Show(pose))
    {
        applicationControlRuntime_->privatePointerVisualError = "cursor_overlay_unavailable";
        throw std::runtime_error("cursor_overlay_unavailable");
    }
}

snowdesktop::control::Value DesktopApp::PrivatePointerState(bool includeTrace) const
{
    using namespace snowdesktop::control;
    const auto& runtime = *applicationControlRuntime_;
    const auto& state = runtime.privatePointer.Current();
    auto result = Object({{"sessionId", String(runtime.privatePointerSessionId)},
        {"enabled", Boolean(state.enabled)}, {"closing", Boolean(state.closing)},
        {"source", String("application-private-pointer")}, {"coordinateSpace", String("physical-screen-pixels")},
        {"monitorId", String(WideToUtf8(runtime.privatePointerMonitor))},
        {"x", Number(state.point.x)}, {"y", Number(state.point.y)}, {"buttons", Number(state.buttons)},
        {"sequence", String(std::to_string(state.sequence))}, {"pendingEvents", Number(static_cast<double>(state.pendingEvents))},
        {"error", String(state.error.empty() ? runtime.privatePointerVisualError : state.error)}, {"hasCapture", Boolean(state.captured != nullptr)},
        {"hasHover", Boolean(state.hovered != nullptr)}, {"hasFocus", Boolean(state.focused != nullptr)}});
    auto keys = Array();
    for (unsigned key = 0; key < state.keys.size(); ++key)
        if (state.keys[key]) keys.array.push_back(Number(key));
    result.object.emplace("keys", std::move(keys));
    if (includeTrace)
    {
        auto trace = Array();
        for (const auto& entry : state.trace)
            trace.array.push_back(Object({{"sequence", String(std::to_string(entry.sequence))},
                {"timeMs", Number(entry.milliseconds)}, {"x", Number(entry.point.x)}, {"y", Number(entry.point.y)},
                {"buttons", Number(entry.buttons)}, {"message", Number(entry.message)}, {"phase", String(entry.phase)},
                {"virtualKey", Number(entry.virtualKey)}, {"codeUnit", Number(entry.codeUnit)}}));
        result.object.emplace("trace", std::move(trace));
    }
    return result;
}

snowdesktop::control::Value DesktopApp::HandlePrivatePointer(std::string_view method, const snowdesktop::control::Value& params)
{
    using namespace snowdesktop::control;
    auto& runtime = *applicationControlRuntime_;
    auto& device = runtime.privatePointer;
    if (method == "input.state")
    { Fields(params, {"includeTrace"}); return PrivatePointerState(Flag(params, "includeTrace", false)); }
    if (method == "input.begin")
    {
        Fields(params, {"monitorId", "x", "y", "showCursor"});
        if (device.Current().enabled || snowdesktop::private_pointer::Active() || mouseDown_ || dragSession_.IsActive() ||
            widgetAction_ != WidgetAction::None || HasActiveContextMenuSession() || runtime.settings.Current().status == "running")
            throw Error("busy", "Another input session or native interaction is active");
        if (desktopStartupPresentationPending_ || popupAnimation_.IsAnimating() || quickNavigationAnimation_.IsAnimating())
            throw Error("not_ready", "Wait for the native presentation to settle");
        const auto monitorId = Utf8ToWide(Text(params, "monitorId"));
        const auto monitors = snowdesktop::screen_capture::Monitors();
        const auto monitor = std::find_if(monitors.begin(), monitors.end(), [&](const auto& value) { return value.id == monitorId; });
        if (monitor == monitors.end()) throw Error("monitor_not_found", "Select a current capture.monitors entry");
        POINT initial{monitor->bounds.left + (monitor->bounds.right - monitor->bounds.left) / 2,
            monitor->bounds.top + (monitor->bounds.bottom - monitor->bounds.top) / 2};
        if ((params.Find("x") != nullptr) != (params.Find("y") != nullptr))
            throw Error("invalid_params", "x and y must be supplied together", -32602);
        if (params.Find("x")) { initial.x = Integer(params, "x", -1000000, 1000000); initial.y = Integer(params, "y", -1000000, 1000000); }
        runtime.privatePointerShowCursor = Flag(params, "showCursor", true);
        runtime.privatePointerMonitor = monitorId; runtime.privatePointerSessionId = SessionId();
        runtime.privatePointerVisualError.clear(); runtime.privatePointerStarted = std::chrono::steady_clock::now();
        device.SetObserver([this](const auto&) { PresentPrivatePointer(); });
        std::string error;
        if (!device.Begin(monitor->bounds, initial, [this](POINT point) { return PrivatePointerWindowAt(point); },
            [this](HWND window) { return OwnsPrivatePointerWindow(window); }, error,
            [](HWND window, POINT screen, POINT& client) {
                return snowdesktop::text_input::MapPrivatePointerPoint(window, screen, client);
            }))
        { device.SetObserver({}); throw Error(error.empty() ? "input_unavailable" : error, "Private pointer session could not start"); }
        if (runtime.privatePointerShowCursor)
        {
            runtime.privatePointerTimerHost = hwnd_;
            runtime.privatePointerTimer = SetTimer(hwnd_, PrivatePointerCursorTimer, 16,
                [](HWND window, UINT, UINT_PTR timer, DWORD) {
                    auto* app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                    if (!app || !app->applicationControlRuntime_) { KillTimer(window, timer); return; }
                    try { app->PresentPrivatePointer(); }
                    catch (...)
                    {
                        app->applicationControlRuntime_->privatePointer.End();
                        app->applicationControlRuntime_->cursor.Hide();
                        KillTimer(window, timer);
                    }
                });
            if (!runtime.privatePointerTimer)
            { device.End(); throw Error("input_unavailable", "Private pointer presentation timer could not start"); }
        }
        PresentPrivatePointer();
        return PrivatePointerState(false);
    }
    if (method == "input.end")
    {
        Fields(params, {"sessionId"});
        if (Text(params, "sessionId") != runtime.privatePointerSessionId || runtime.privatePointerSessionId.empty())
            throw Error("input_session_not_found", "This private input session is no longer current");
        device.End(); runtime.cursor.Hide();
        return PrivatePointerState(false);
    }
    if (method == "input.keyboard") Fields(params, {"sessionId", "type", "key", "text"});
    else Fields(params, {"sessionId", "type", "x", "y", "button", "delta"});
    if (Text(params, "sessionId") != runtime.privatePointerSessionId || !device.Current().enabled || device.Current().closing)
        throw Error("input_session_not_found", "Begin a current private input session first");
    const auto currentMonitors = snowdesktop::screen_capture::Monitors();
    if (std::none_of(currentMonitors.begin(), currentMonitors.end(), [&](const auto& monitor) {
        return monitor.id == runtime.privatePointerMonitor && EqualRect(&monitor.bounds, &device.Current().bounds); }))
    {
        device.End(); runtime.cursor.Hide();
        throw Error("display_changed", "The private pointer monitor changed; begin a new session");
    }
    const auto type = Text(params, "type");
    if (method == "input.keyboard")
    {
        std::string error; bool accepted = false;
        if (type == "text")
        {
            if (params.Find("key")) throw Error("invalid_params", "text input cannot include a key", -32602);
            const auto* text = params.Find("text");
            if (!text || !text->IsString() || text->string.empty() || text->string.size() > 4096 || text->string.find('\0') != std::string::npos)
                throw Error("invalid_params", "text must contain 1..4096 UTF-8 bytes and no NUL", -32602);
            accepted = device.Text(Utf8ToWide(text->string), error);
        }
        else if (type == "down" || type == "up")
        {
            if (params.Find("text")) throw Error("invalid_params", "key input cannot include text", -32602);
            const unsigned key = static_cast<unsigned>(Integer(params, "key", 1, 255));
            accepted = type == "down" ? device.KeyDown(key, error) : device.KeyUp(key, error);
        }
        else throw Error("invalid_params", "keyboard type must be down, up or text", -32602);
        if (!accepted) throw Error(error.empty() ? "input_rejected" : error, "Private keyboard event was rejected");
        return PrivatePointerState(false);
    }
    if ((type != "move" && (params.Find("x") || params.Find("y"))) ||
        (type != "down" && type != "up" && params.Find("button")) || (type != "wheel" && params.Find("delta")))
        throw Error("invalid_params", "Pointer fields must match the event type", -32602);
    std::string error; bool accepted = false;
    if (type == "move") accepted = device.Move({Integer(params, "x", -1000000, 1000000), Integer(params, "y", -1000000, 1000000)}, error);
    else if (type == "down" || type == "up")
    {
        const auto text = Text(params, "button");
        snowdesktop::private_pointer::Button button;
        if (text == "left") button = snowdesktop::private_pointer::Button::Left;
        else if (text == "right") button = snowdesktop::private_pointer::Button::Right;
        else if (text == "middle") button = snowdesktop::private_pointer::Button::Middle;
        else throw Error("invalid_params", "button must be left, right or middle", -32602);
        accepted = type == "down" ? device.Down(button, error) : device.Up(button, error);
    }
    else if (type == "wheel") accepted = device.Wheel(Integer(params, "delta", -12000, 12000), error);
    else throw Error("invalid_params", "type must be move, down, up or wheel", -32602);
    if (!accepted) throw Error(error.empty() ? "input_rejected" : error, "Private pointer event was rejected");
    return PrivatePointerState(false);
}
