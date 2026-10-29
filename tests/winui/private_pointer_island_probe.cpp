#include "winui/windows_compat.h"
#include "winui/winui_runtime.h"
#include "common/utils.h"
#include "platform/control/control_protocol.h"
#include "platform/deployment_context.h"
#include "data/data_paths.h"
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// This is an explicit manual diagnostic, not a production input adapter. A
// successful Win32 send is only a transport observation. Acceptance requires
// XAML's real pointer events and native control properties to change while the
// actual system input remains unchanged. No UIA, VSM, private ABI or system
// input injection is used here.
namespace
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = winrt::Microsoft::UI::Xaml::Controls;
namespace i = winrt::Microsoft::UI::Xaml::Input;
using namespace snowdesktop::control;
using snowdesktop::winui::WinUiRuntime;

std::string Handle(HWND window)
{ return std::to_string(reinterpret_cast<std::uintptr_t>(window)); }
std::string ModulePath(HMODULE module)
{
    if (!module) return {};
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length); return WideToUtf8(path);
}
Value Window(HWND window)
{
    DWORD process = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
    wchar_t name[256]{};
    if (window) GetClassNameW(window, name, static_cast<int>(std::size(name)));
    RECT rect{};
    if (window) GetWindowRect(window, &rect);
    return Object({{"hwnd", String(Handle(window))}, {"class", String(WideToUtf8(name))},
        {"processId", Number(process)}, {"threadId", Number(thread)},
        {"parent", String(Handle(window ? GetParent(window) : nullptr))},
        {"visible", Boolean(window && IsWindowVisible(window))},
        {"left", Number(rect.left)}, {"top", Number(rect.top)},
        {"right", Number(rect.right)}, {"bottom", Number(rect.bottom)}});
}
Value GuiThread(DWORD thread)
{
    GUITHREADINFO info{}; info.cbSize = sizeof(info);
    const bool ok = GetGUIThreadInfo(thread, &info) != FALSE;
    return Object({{"threadId", Number(thread)}, {"available", Boolean(ok)},
        {"active", String(Handle(info.hwndActive))}, {"focus", String(Handle(info.hwndFocus))},
        {"capture", String(Handle(info.hwndCapture))}});
}
Value SystemInput(const std::set<DWORD>& threads)
{
    POINT cursor{};
    const bool cursorKnown = ::GetCursorPos(&cursor) != FALSE;
    auto ownThreads = Array();
    for (const DWORD thread : threads) ownThreads.array.push_back(GuiThread(thread));
    return Object({{"cursorKnown", Boolean(cursorKnown)}, {"x", Number(cursor.x)}, {"y", Number(cursor.y)},
        {"foreground", String(Handle(::GetForegroundWindow()))}, {"focus", String(Handle(::GetFocus()))},
        {"capture", String(Handle(::GetCapture()))}, {"foregroundGui", GuiThread(0)},
        {"ownedThreads", std::move(ownThreads)}});
}
bool SameInput(const Value& before, const Value& after)
{
    const auto invariant = [](Value value) {
        // Creating WinUI's GUI queues can change availability and a background
        // thread's active-window bookkeeping. Neither is the user's active
        // input target. Keep all original fields in the report, but compare
        // cursor, foreground GUI, and every observed thread's focus/capture.
        const auto threads = value.object.find("ownedThreads");
        if (threads != value.object.end())
            for (auto& thread : threads->second.array)
            { thread.object.erase("available"); thread.object.erase("active"); }
        return Serialize(value);
    };
    return before.Find("cursorKnown")->boolean && after.Find("cursorKnown")->boolean &&
        before.Find("foregroundGui")->Find("available")->boolean && after.Find("foregroundGui")->Find("available")->boolean &&
        invariant(before) == invariant(after);
}
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

struct Apartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Host
{
    WinUiRuntime runtime;
    HWND window = nullptr;
    bool closed = false;
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_CLOSE) { if (host) host->closed = true; return 0; }
        if (host) host->runtime.HandleWindowMessage(message, wp, lp);
        return DefWindowProcW(window, message, wp, lp);
    }
    void PumpUntil(const std::function<bool()>& ready, unsigned timeoutMs = 3000)
    {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (!ready())
        {
            Require(!closed, "probe_window_closed");
            Require(GetTickCount64() < deadline, "probe_message_pump_timeout");
            MSG message{};
            unsigned count = 0;
            while (count++ < 128 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                Require(message.message != WM_QUIT, "probe_received_quit");
                if (!runtime.PreTranslateMessage(&message) && !runtime.ProcessTabNavigation(&message))
                { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (!ready()) MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    void Settle(unsigned milliseconds)
    {
        const auto until = GetTickCount64() + milliseconds;
        PumpUntil([&] { return GetTickCount64() >= until; }, milliseconds + 2000);
    }
    void Close() noexcept
    {
        runtime.Detach();
        if (window) { DestroyWindow(window); window = nullptr; }
        runtime.Shutdown();
    }
    ~Host() { Close(); }
};

struct Metrics
{
    unsigned entered = 0, exited = 0, moved = 0, pressed = 0, released = 0, wheel = 0;
    unsigned cancelled = 0, captureLost = 0, clicks = 0, toggled = 0, viewChanged = 0;
    bool insideEnter = false, insidePress = false, insideRelease = false, insideWheel = false;
    bool pressedButtonState = false, releasedButtonState = false;
    Value events = Array();
};
Value Counts(const Metrics& value)
{
    return Object({{"entered", Number(value.entered)}, {"exited", Number(value.exited)},
        {"moved", Number(value.moved)}, {"pressed", Number(value.pressed)}, {"released", Number(value.released)},
        {"wheel", Number(value.wheel)}, {"cancelled", Number(value.cancelled)}, {"captureLost", Number(value.captureLost)},
        {"clicks", Number(value.clicks)}, {"toggled", Number(value.toggled)}, {"viewChanged", Number(value.viewChanged)},
        {"insideEnter", Boolean(value.insideEnter)}, {"insidePress", Boolean(value.insidePress)},
        {"insideRelease", Boolean(value.insideRelease)}, {"insideWheel", Boolean(value.insideWheel)},
        {"pressedButtonState", Boolean(value.pressedButtonState)}, {"releasedButtonState", Boolean(value.releasedButtonState)}});
}
void ObservePointer(const x::UIElement& element, const std::shared_ptr<Metrics>& metrics)
{
    const auto observe = [&](const x::RoutedEvent& routed, const char* name, unsigned Metrics::*counter) {
        element.AddHandler(routed, winrt::box_value(i::PointerEventHandler([metrics, name, counter](
            const winrt::Windows::Foundation::IInspectable& sender, const i::PointerRoutedEventArgs& args) {
            ++(metrics.get()->*counter);
            if (metrics->events.array.size() >= 256) return;
            try
            {
                const auto owner = sender.as<x::FrameworkElement>();
                const auto point = args.GetCurrentPoint(owner);
                const auto position = point.Position();
                const bool inside = position.X >= 0 && position.Y >= 0 &&
                    position.X < owner.ActualWidth() && position.Y < owner.ActualHeight();
                const bool left = point.Properties().IsLeftButtonPressed();
                if (counter == &Metrics::entered) metrics->insideEnter = metrics->insideEnter || inside;
                if (counter == &Metrics::pressed)
                { metrics->insidePress = metrics->insidePress || inside; metrics->pressedButtonState = metrics->pressedButtonState || left; }
                if (counter == &Metrics::released)
                { metrics->insideRelease = metrics->insideRelease || inside; metrics->releasedButtonState = metrics->releasedButtonState || !left; }
                if (counter == &Metrics::wheel) metrics->insideWheel = metrics->insideWheel || inside;
                POINT systemCursor{};
                const bool systemKnown = ::GetCursorPos(&systemCursor) != FALSE;
                metrics->events.array.push_back(Object({{"event", String(name)}, {"timeMs", String(std::to_string(GetTickCount64()))},
                    {"threadId", Number(GetCurrentThreadId())}, {"pointerId", Number(point.PointerId())},
                    {"device", Number(static_cast<int>(args.Pointer().PointerDeviceType()))},
                    {"x", Number(position.X)}, {"y", Number(position.Y)}, {"withinControl", Boolean(inside)},
                    {"leftDown", Boolean(left)}, {"handled", Boolean(args.Handled())},
                    {"systemCursorKnown", Boolean(systemKnown)}, {"systemX", Number(systemCursor.x)}, {"systemY", Number(systemCursor.y)},
                    {"foreground", String(Handle(::GetForegroundWindow()))}, {"threadFocus", String(Handle(::GetFocus()))},
                    {"threadCapture", String(Handle(::GetCapture()))}}));
            }
            catch (const winrt::hresult_error& error)
            {
                metrics->events.array.push_back(Object({{"event", String(name)},
                    {"pointError", String(winrt::to_string(error.message()))}}));
            }
        })), true);
    };
    observe(x::UIElement::PointerEnteredEvent(), "entered", &Metrics::entered);
    observe(x::UIElement::PointerExitedEvent(), "exited", &Metrics::exited);
    observe(x::UIElement::PointerMovedEvent(), "moved", &Metrics::moved);
    observe(x::UIElement::PointerPressedEvent(), "pressed", &Metrics::pressed);
    observe(x::UIElement::PointerReleasedEvent(), "released", &Metrics::released);
    observe(x::UIElement::PointerWheelChangedEvent(), "wheel", &Metrics::wheel);
    observe(x::UIElement::PointerCanceledEvent(), "cancelled", &Metrics::cancelled);
    observe(x::UIElement::PointerCaptureLostEvent(), "captureLost", &Metrics::captureLost);
}
struct Fixture
{
    c::StackPanel root;
    c::Button button;
    c::ToggleSwitch toggle;
    c::ScrollViewer scroll;
    std::shared_ptr<Metrics> buttonEvents = std::make_shared<Metrics>();
    std::shared_ptr<Metrics> toggleEvents = std::make_shared<Metrics>();
    std::shared_ptr<Metrics> scrollEvents = std::make_shared<Metrics>();
    Fixture()
    {
        root.Margin({24, 24, 24, 24}); root.Spacing(16);
        c::TextBlock title; title.Text(L"Private pointer: real WinUI event probe"); title.FontSize(20);
        root.Children().Append(title);
        button.Content(winrt::box_value(L"Native Button")); button.Width(320); button.Height(48);
        button.HorizontalAlignment(x::HorizontalAlignment::Left);
        root.Children().Append(button);
        c::TextBlock toggleTitle; toggleTitle.Text(L"Native ToggleSwitch"); root.Children().Append(toggleTitle);
        toggle.Width(320); toggle.MinHeight(48); toggle.HorizontalAlignment(x::HorizontalAlignment::Left);
        toggle.OnContent(winrt::box_value(L"On")); toggle.OffContent(winrt::box_value(L"Off"));
        root.Children().Append(toggle);
        c::TextBlock scrollTitle; scrollTitle.Text(L"Native ScrollViewer"); root.Children().Append(scrollTitle);
        scroll.Width(420); scroll.Height(220); scroll.HorizontalAlignment(x::HorizontalAlignment::Left);
        scroll.VerticalScrollMode(c::ScrollMode::Enabled); scroll.HorizontalScrollMode(c::ScrollMode::Disabled);
        scroll.VerticalScrollBarVisibility(c::ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled); scroll.ZoomMode(c::ZoomMode::Disabled);
        c::StackPanel rows;
        for (unsigned index = 0; index < 30; ++index)
        {
            c::TextBlock row; row.Text(winrt::hstring(L"Actual XAML scroll row " + std::to_wstring(index + 1)));
            row.Height(32); rows.Children().Append(row);
        }
        scroll.Content(rows); root.Children().Append(scroll);
        ObservePointer(button, buttonEvents); ObservePointer(toggle, toggleEvents); ObservePointer(scroll, scrollEvents);
        button.Click([metrics = buttonEvents](const auto&, const auto&) { ++metrics->clicks; });
        toggle.Toggled([metrics = toggleEvents](const auto&, const auto&) { ++metrics->toggled; });
        scroll.ViewChanged([metrics = scrollEvents](const auto&, const auto&) { ++metrics->viewChanged; });
    }
    Value State() const
    {
        return Object({{"button", Object({{"isPointerOver", Boolean(button.IsPointerOver())},
            {"isPressed", Boolean(button.IsPressed())}, {"events", Counts(*buttonEvents)}})},
            {"toggle", Object({{"isOn", Boolean(toggle.IsOn())}, {"events", Counts(*toggleEvents)}})},
            {"scroll", Object({{"verticalOffset", Number(scroll.VerticalOffset())},
                {"scrollableHeight", Number(scroll.ScrollableHeight())}, {"events", Counts(*scrollEvents)}})}});
    }
    POINT Point(const x::FrameworkElement& control, HWND island, bool switchTrack = false) const
    {
        const float localX = static_cast<float>(switchTrack ? 20.0 : control.ActualWidth() / 2.0);
        const float localY = static_cast<float>(control.ActualHeight() / 2.0);
        const auto relative = control.TransformToVisual(root).TransformPoint({localX, localY});
        // The root's margin is outside its own coordinate space.
        const double scale = root.XamlRoot().RasterizationScale();
        POINT origin{}; Require(ClientToScreen(island, &origin) != FALSE, "probe_island_origin_unavailable");
        return {origin.x + static_cast<LONG>(std::lround((relative.X + root.Margin().Left) * scale)),
            origin.y + static_cast<LONG>(std::lround((relative.Y + root.Margin().Top) * scale))};
    }
};
struct Tree
{
    HWND host = nullptr, island = nullptr;
    std::vector<HWND> windows;
    std::set<DWORD> threads;
    static BOOL CALLBACK Visit(HWND window, LPARAM parameter)
    {
        auto& tree = *reinterpret_cast<Tree*>(parameter);
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && tree.windows.size() < 64)
        { tree.windows.push_back(window); tree.threads.insert(thread); }
        return TRUE;
    }
    Tree(HWND parent, HWND child) : host(parent), island(child)
    {
        threads.insert(GetCurrentThreadId());
        windows.push_back(island);
        DWORD process = 0;
        threads.insert(GetWindowThreadProcessId(island, &process));
        EnumChildWindows(island, Visit, reinterpret_cast<LPARAM>(this));
    }
    bool Owns(HWND window) const
    {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        return thread && process == GetCurrentProcessId() && IsWindow(window) &&
            (window == island || IsChild(island, window)) && IsChild(host, island);
    }
    Value Describe() const
    {
        auto values = Array();
        for (HWND window : windows)
        {
            auto item = Window(window);
            item.object.emplace("withinOwnedIsland", Boolean(Owns(window)));
            values.array.push_back(std::move(item));
        }
        return values;
    }
};

struct Packet
{
    const char* name;
    UINT message;
    WPARAM wp;
    POINT screen;
};
Value Send(Host& host, const Tree& tree, HWND receiver, const Packet& packet, bool post)
{
    Require(tree.Owns(receiver), "probe_receiver_left_owned_island");
    POINT coordinate = packet.screen;
    if (packet.message != WM_MOUSEWHEEL)
        Require(ScreenToClient(receiver, &coordinate) != FALSE, "probe_receiver_coordinates_unavailable");
    Require(coordinate.x >= SHRT_MIN && coordinate.x <= SHRT_MAX && coordinate.y >= SHRT_MIN && coordinate.y <= SHRT_MAX,
        "probe_coordinates_exceed_win32_mouse_message");
    const LPARAM lp = MAKELPARAM(static_cast<WORD>(coordinate.x), static_cast<WORD>(coordinate.y));
    const auto before = SystemInput(tree.threads);
    bool sent = false;
    DWORD error = 0;
    DWORD_PTR answer = 0;
    DWORD senderThread = GetCurrentThreadId();
    const auto started = GetTickCount64();
    if (post)
    {
        SetLastError(ERROR_SUCCESS);
        sent = PostMessageW(receiver, packet.message, packet.wp, lp) != FALSE;
        error = GetLastError();
    }
    else
    {
        // Use a sender without a window, so even an Island child on the UI
        // thread is tested with an actual bounded cross-thread send. The UI
        // thread keeps its normal message pump running during the experiment.
        std::atomic<bool> done{false};
        std::thread sender([&] {
            senderThread = GetCurrentThreadId();
            if (!tree.Owns(receiver)) { error = ERROR_INVALID_WINDOW_HANDLE; done.store(true); return; }
            SetLastError(ERROR_SUCCESS);
            sent = SendMessageTimeoutW(receiver, packet.message, packet.wp, lp,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 500, &answer) != 0;
            error = GetLastError(); done.store(true);
        });
        try { host.PumpUntil([&] { return done.load(); }, 2000); }
        catch (...) { sender.join(); throw; }
        sender.join();
    }
    host.Settle(90);
    const auto after = SystemInput(tree.threads);
    return Object({{"action", String(packet.name)}, {"message", Number(packet.message)},
        {"screenX", Number(packet.screen.x)}, {"screenY", Number(packet.screen.y)},
        {"clientX", Number(coordinate.x)}, {"clientY", Number(coordinate.y)},
        {"senderThreadId", Number(senderThread)},
        {"elapsedMs", Number(static_cast<double>(GetTickCount64() - started))},
        {"sent", Boolean(sent)}, {"win32Error", Number(error)}, {"messageResult", String(std::to_string(answer))},
        {"systemInputUnchanged", Boolean(SameInput(before, after))}, {"beforeSystemInput", before},
        {"afterSystemInput", after}});
}
RECT Placement()
{
    POINT cursor{}; Require(::GetCursorPos(&cursor) != FALSE, "probe_system_cursor_unavailable");
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{}; info.cbSize = sizeof(info);
    Require(GetMonitorInfoW(monitor, &info) != FALSE, "probe_monitor_unavailable");
    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(680, static_cast<int>(dpi), 96);
    const int height = MulDiv(640, static_cast<int>(dpi), 96);
    Require(info.rcWork.right - info.rcWork.left >= width + 32 && info.rcWork.bottom - info.rcWork.top >= height + 32,
        "probe_monitor_too_small");
    const POINT corners[] = {{info.rcWork.left + 16, info.rcWork.top + 16},
        {info.rcWork.right - width - 16, info.rcWork.top + 16},
        {info.rcWork.left + 16, info.rcWork.bottom - height - 16},
        {info.rcWork.right - width - 16, info.rcWork.bottom - height - 16}};
    for (const auto point : corners)
    {
        const RECT rect{point.x, point.y, point.x + width, point.y + height};
        if (!PtInRect(&rect, cursor)) return rect;
    }
    throw std::runtime_error("probe_cannot_place_away_from_system_cursor");
}
void Save(const std::filesystem::path& output, const Value& report)
{
    const auto text = Serialize(report) + "\n";
    Require(text.size() <= 16 * 1024 * 1024, "probe_report_too_large");
    const HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "probe_report_create_failed");
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
        written == text.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    Require(success, "probe_report_write_failed");
}
}

// Called only by the explicit manual mode of the existing native-host test
// executable. Ordinary CTest modes never construct or show this window.
int RunWinUiPrivatePointerIslandProbe(const std::filesystem::path& output)
{
    auto report = Object({{"schemaVersion", Number(1)}, {"source", String("real-winui-island-pointer-probe")},
        {"status", String("setup-failed")}, {"supported", Boolean(false)}, {"processId", Number(GetCurrentProcessId())},
        {"uiThreadId", Number(GetCurrentThreadId())}, {"systemInjection", Boolean(false)},
        {"uiaInvocation", Boolean(false)}, {"visualStateOverride", Boolean(false)},
        {"privateAbi", Boolean(false)},
        {"scope", String("WM_MOUSEMOVE/WM_LBUTTONDOWN/WM_LBUTTONUP/WM_MOUSEWHEEL to the listed owned Island HWNDs on this runtime")}});
    int result = 1;
    try
    {
        Require(output.is_absolute() && !std::filesystem::exists(output) && std::filesystem::is_directory(output.parent_path()),
            "probe_requires_new_absolute_report_path");
        // The product Application loads font/settings resources. Require an
        // unpackaged portable copy with no pre-existing data or migration file
        // before initializing it, so this diagnostic cannot read a user profile.
        const auto& deployment = snowdesktop::deployment::GetRuntimeDeploymentContext();
        Require(deployment.kind == snowdesktop::deployment::RuntimeDeploymentKind::Portable && !deployment.explicitContext &&
            !snowdesktop::deployment::IsPackaged(), "probe_requires_isolated_portable_runtime_copy");
        const std::filesystem::path executableDirectory(GetExecutableDirectoryPath());
        Require(!std::filesystem::exists(executableDirectory / L"data") &&
            !std::filesystem::exists(executableDirectory / L"SnowDesktop.general.json"), "probe_profile_is_not_empty");
        report.object.emplace("executableDirectory", String(WideToUtf8(executableDirectory.wstring())));
        report.object.emplace("executable", String(ModulePath(GetModuleHandleW(nullptr))));
        Apartment apartment; winrt::check_hresult(apartment.result);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Host host;
        const auto initialSystem = SystemInput({GetCurrentThreadId()});
        report.object.emplace("beforeSetupSystemInput", initialSystem);
        if (!host.runtime.Initialize()) throw std::runtime_error(WideToUtf8(host.runtime.LastError()));
        WNDCLASSW definition{}; definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpfnWndProc = Host::Procedure; definition.lpszClassName = L"SnowDesktop.WinUI.PrivatePointerProbe";
        Require(RegisterClassW(&definition) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "probe_window_class_failed");
        const auto rect = Placement();
        host.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
            definition.lpszClassName, L"SnowDesktop WinUI private pointer probe", WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, definition.hInstance, &host);
        Require(host.window != nullptr, "probe_window_creation_failed");
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        Fixture first;
        if (!host.runtime.Attach(host.window, first.root)) throw std::runtime_error(WideToUtf8(host.runtime.LastError()));
        host.PumpUntil([&] { first.root.UpdateLayout(); return first.button.ActualWidth() > 10 && first.scroll.ScrollableHeight() > 10; });
        host.Settle(250);
        const Tree initialTree(host.window, host.runtime.IslandWindow());
        report.object.emplace("host", Window(host.window));
        report.object.emplace("island", Window(host.runtime.IslandWindow()));
        report.object.emplace("winuiModule", String(ModulePath(GetModuleHandleW(L"Microsoft.UI.Xaml.dll"))));
        report.object.emplace("dpi", Number(GetDpiForWindow(host.window)));
        report.object.emplace("rasterizationScale", Number(first.root.XamlRoot().RasterizationScale()));
        report.object.emplace("receiverTree", initialTree.Describe());
        report.object.emplace("receiverCount", Number(static_cast<double>(initialTree.windows.size())));
        report.object.emplace("maximumTestedReceivers", Number(8));
        report.object.emplace("allEnumeratedReceiversTested", Boolean(initialTree.windows.size() <= 8));
        const auto afterSetup = SystemInput({GetCurrentThreadId()});
        report.object.emplace("afterSetupSystemInput", afterSetup);
        const bool setupInputUnchanged = SameInput(initialSystem, afterSetup);
        report.object.emplace("setupSystemInputUnchanged", Boolean(setupInputUnchanged));
        report.object.emplace("trials", Array());
        auto& trials = report.object.at("trials");
        bool anySupported = false;
        std::size_t candidates = 0;
        for (HWND receiver : initialTree.windows)
        {
            if (candidates++ >= 8) break;
            for (const bool post : {false, true})
            {
                Fixture fixture;
                if (!host.runtime.Attach(host.window, fixture.root)) throw std::runtime_error(WideToUtf8(host.runtime.LastError()));
                host.PumpUntil([&] { fixture.root.UpdateLayout(); return fixture.button.ActualWidth() > 10 && fixture.scroll.ScrollableHeight() > 10; });
                host.Settle(150);
                const Tree tree(host.window, host.runtime.IslandWindow());
                auto trial = Object({{"receiver", Window(receiver)}, {"transport", String(post ? "PostMessage" : "SendMessageTimeout")},
                    {"eligible", Boolean(tree.Owns(receiver))}, {"initialState", fixture.State()}});
                if (!tree.Owns(receiver))
                { trial.object.emplace("status", String("receiver-recreated")); trials.array.push_back(std::move(trial)); continue; }
                auto actions = Array();
                const auto trialInitialSystem = SystemInput({GetCurrentThreadId()});
                POINT actualCursor{}; RECT hostBounds{};
                const bool cursorOutside = ::GetCursorPos(&actualCursor) && GetWindowRect(host.window, &hostBounds) &&
                    !PtInRect(&hostBounds, actualCursor);
                const bool initialUnchanged = SameInput(initialSystem, trialInitialSystem);
                trial.object.emplace("initialSystemInput", trialInitialSystem);
                trial.object.emplace("initialSystemInputUnchanged", Boolean(initialUnchanged));
                trial.object.emplace("systemCursorOutsideProbe", Boolean(cursorOutside));
                bool unchanged = initialUnchanged && cursorOutside, transportWorked = true;
                bool buttonOver = false, buttonPressed = false;
                const auto buttonPoint = fixture.Point(fixture.button, tree.island);
                const auto togglePoint = fixture.Point(fixture.toggle, tree.island, true);
                const auto scrollPoint = fixture.Point(fixture.scroll, tree.island);
                POINT outside{}; Require(ClientToScreen(tree.island, &outside) != FALSE, "probe_leave_origin_unavailable");
                outside.x += static_cast<LONG>(std::lround((fixture.root.ActualWidth() + 16) * fixture.root.XamlRoot().RasterizationScale()));
                outside.y += 8;
                const auto apply = [&](const Packet& packet) {
                    auto action = Send(host, tree, receiver, packet, post);
                    unchanged = unchanged && action.Find("systemInputUnchanged")->boolean;
                    transportWorked = transportWorked && action.Find("sent")->boolean;
                    action.object.emplace("xamlState", fixture.State());
                    actions.array.push_back(std::move(action));
                };
                apply({"button.hover", WM_MOUSEMOVE, 0, buttonPoint}); buttonOver = fixture.button.IsPointerOver();
                apply({"button.down", WM_LBUTTONDOWN, MK_LBUTTON, buttonPoint}); buttonPressed = fixture.button.IsPressed();
                apply({"button.up", WM_LBUTTONUP, 0, buttonPoint});
                apply({"button.leave", WM_MOUSEMOVE, 0, outside});
                apply({"toggle.hover", WM_MOUSEMOVE, 0, togglePoint});
                apply({"toggle.down", WM_LBUTTONDOWN, MK_LBUTTON, togglePoint});
                apply({"toggle.up", WM_LBUTTONUP, 0, togglePoint});
                apply({"toggle.leave", WM_MOUSEMOVE, 0, outside});
                apply({"scroll.hover", WM_MOUSEMOVE, 0, scrollPoint});
                apply({"scroll.wheel", WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-3 * WHEEL_DELTA)), scrollPoint});
                host.Settle(250);
                apply({"scroll.leave", WM_MOUSEMOVE, 0, outside});
                const bool buttonWorks = buttonOver && buttonPressed && !fixture.button.IsPressed() && !fixture.button.IsPointerOver() &&
                    fixture.buttonEvents->entered && fixture.buttonEvents->exited && fixture.buttonEvents->pressed &&
                    fixture.buttonEvents->released && fixture.buttonEvents->clicks && fixture.buttonEvents->insideEnter &&
                    fixture.buttonEvents->insidePress && fixture.buttonEvents->insideRelease &&
                    fixture.buttonEvents->pressedButtonState && fixture.buttonEvents->releasedButtonState;
                const bool toggleWorks = fixture.toggle.IsOn() && fixture.toggleEvents->toggled && fixture.toggleEvents->entered &&
                    fixture.toggleEvents->exited && fixture.toggleEvents->pressed && fixture.toggleEvents->released &&
                    fixture.toggleEvents->insideEnter && fixture.toggleEvents->insidePress && fixture.toggleEvents->insideRelease &&
                    fixture.toggleEvents->pressedButtonState && fixture.toggleEvents->releasedButtonState;
                const bool scrollWorks = fixture.scroll.VerticalOffset() > 0 && fixture.scrollEvents->viewChanged &&
                    fixture.scrollEvents->entered && fixture.scrollEvents->exited && fixture.scrollEvents->wheel &&
                    fixture.scrollEvents->insideEnter && fixture.scrollEvents->insideWheel;
                const bool supported = buttonWorks && toggleWorks && scrollWorks && unchanged && setupInputUnchanged && transportWorked;
                anySupported = anySupported || supported;
                trial.object.emplace("status", String(supported ? "supported" : !unchanged ? "system-input-changed" : "not-supported"));
                trial.object.emplace("buttonWorks", Boolean(buttonWorks)); trial.object.emplace("toggleWorks", Boolean(toggleWorks));
                trial.object.emplace("scrollWorks", Boolean(scrollWorks)); trial.object.emplace("systemInputUnchanged", Boolean(unchanged));
                trial.object.emplace("transportWorked", Boolean(transportWorked));
                trial.object.emplace("actions", std::move(actions)); trial.object.emplace("finalState", fixture.State());
                trial.object.emplace("buttonEvents", fixture.buttonEvents->events);
                trial.object.emplace("toggleEvents", fixture.toggleEvents->events);
                trial.object.emplace("scrollEvents", fixture.scrollEvents->events);
                trials.array.push_back(std::move(trial));
            }
        }
        host.Close();
        const auto afterCleanup = SystemInput({GetCurrentThreadId()});
        const bool cleanupInputUnchanged = SameInput(initialSystem, afterCleanup);
        report.object.emplace("afterCleanupSystemInput", afterCleanup);
        report.object.emplace("cleanupSystemInputUnchanged", Boolean(cleanupInputUnchanged));
        anySupported = anySupported && cleanupInputUnchanged;
        report.object["supported"] = Boolean(anySupported);
        report.object["status"] = String(anySupported ? "supported" : "not-supported");
        result = anySupported ? 0 : 3;
    }
    catch (const winrt::hresult_error& error)
    {
        report.object.emplace("error", String(winrt::to_string(error.message())));
        report.object.emplace("hresult", String(std::to_string(error.code().value)));
    }
    catch (const std::exception& error) { report.object.emplace("error", String(error.what())); }
    catch (...) { report.object.emplace("error", String("probe_unexpected_failure")); }
    try { Save(output, report); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "WinUI private-pointer diagnostic: " << report.Find("status")->string << "; report="
        << WideToUtf8(output.wstring()) << '\n';
    return result;
}
