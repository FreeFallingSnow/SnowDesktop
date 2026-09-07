#include "settings_ipc_channel.h"
#include "settings_ipc_values.h"
#include "settings_process.h"

#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
using namespace snowdesktop::settings_ipc;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
template<class F> void Reject(F operation, const char* message)
{
    try { operation(); Check(false, message); }
    catch (const ProtocolError&) {}
}
HANDLE CurrentProcessHandle()
{
    HANDLE result = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
        GetCurrentProcess(), &result, SYNCHRONIZE, FALSE, 0))
        throw std::runtime_error("test process handle failed");
    return result;
}

void TestCodec()
{
    // These types exercise Unicode paths, optional values, wide counters and
    // nested metadata used by settings and component editor snapshots.
    using Value = std::tuple<std::wstring, std::uint64_t,
        std::vector<std::optional<std::string>>, std::map<std::string, double>>;
    const Value original{L"中文 / 日本語 / \U0001F3B5", UINT64_MAX,
        {"", std::nullopt, std::string("a\0b", 3)}, {{"scale", 0.75}}};
    auto encoded = Pack(original);
    Check(Unpack<Value>(encoded) == original, "IPC value round trip preserves Unicode and exact counters");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        Reject([&] { (void)Unpack<Value>(std::span(encoded).first(size)); },
            "truncated snapshots must be rejected before dispatch");
    encoded.push_back(std::byte{});
    Reject([&] { (void)Unpack<Value>(encoded); }, "trailing snapshot fields reject protocol mismatch");
    Reject([] { (void)Unpack<bool>(Pack(std::uint8_t{2})); }, "invalid boolean is rejected");
    Reject([] { (void)Unpack<std::string>(Pack(UINT32_MAX)); }, "oversized string rejected before allocation");
    Reject([] { (void)Unpack<std::vector<std::string>>(Pack(UINT32_MAX)); },
        "oversized collection rejected before allocation");
    Reject([] { (void)Pack(std::numeric_limits<double>::infinity()); },
        "nonfinite slider values cannot cross IPC");
    Reject([] { (void)Unpack<std::filesystem::path>(Pack(std::wstring(L"a\0b", 3))); },
        "embedded null paths cannot change the filesystem operation target");
    Reject([] { (void)Unpack<std::map<std::string, int>>(Pack(std::uint32_t{2},
        std::string("a"), 1, std::string("a"), 2)); }, "duplicate metadata keys rejected");

    snowdesktop::SettingsSnapshot settings;
    settings.generation = 17;
    settings.revision = UINT64_MAX;
    settings.externalReplacementPending = true;
    settings.values.general.animationMode = 1;
    settings.values.general.popupAnimationEffect = 1;
    settings.values.general.animationSpeed = 2;
    settings.values.general.animationFrameLimit = 120;
    settings.values.general.animationEnergySaver = false;
    settings.values.general.animationOnBattery = true;
    settings.values.dock.floatingEdgeSwipeBlockFullscreen = true;
    settings.values.dock.hoverEffect = 1;
    settings.values.dock.hoverScale = 1.75f;
    settings.values.dock.launchEffect = 2;
    settings.values.dock.windowEffect = 3;
    settings.values.general.language[0] = 'z';
    settings.values.general.language[1] = 'h';
    settings.values.general.language[2] = '\0';
    settings.values.dock.systemTaskbarShellUi.enabled = true;
    settings.values.dock.systemTaskbarShellUi.appearance.widgetEdgeHighlightWidth = 3.5f;
    settings.values.desktop.iconBeautify.filterTintR = 0.123f;
    settings.values.category.rules.push_back({L"中文", L"文档", L"txt,md"});
    const auto restored = Unpack<snowdesktop::SettingsSnapshot>(Pack(settings));
    Check(restored.values.general.animationMode == 1 &&
        restored.values.general.popupAnimationEffect == 1 &&
        restored.values.general.animationSpeed == 2 &&
        restored.values.general.animationFrameLimit == 120 &&
        !restored.values.general.animationEnergySaver &&
        restored.values.general.animationOnBattery,
        "animation preferences reach the independent settings process intact");
    Check(restored.externalReplacementPending && restored.revision == UINT64_MAX &&
        restored.values.dock == settings.values.dock &&
        restored.values.desktop.iconBeautify.filterTintR == 0.123f &&
        restored.values.category.rules.front().customLabel == L"文档" &&
        std::string(restored.values.general.language) == "zh",
        "controller IPC preserves replacement marker, draft fields and language array");

    using namespace snowdesktop::widget_runtime;
    WidgetSettingsSnapshot widget;
    widget.widgetId = L"music-1";
    widget.generation = 3;
    widget.revision = 9;
    WidgetSettingFieldState field;
    field.schema.rawType = "password";
    field.schema.key = "secret";
    field.currentValue = MakeWidgetSettingString("must-not-leave-host");
    field.defaultValue = field.currentValue;
    field.opaque.configured = true;
    field.opaque.displayLabel = "must-not-leave-host";
    widget.fields.push_back(field);
    PrepareWidgetSettingsSnapshot(widget);
    const auto wire = Pack(widget);
    const std::string serialized(reinterpret_cast<const char*>(wire.data()), wire.size());
    Check(serialized.find("must-not-leave-host") == std::string::npos &&
        Unpack<WidgetSettingsSnapshot>(wire) == widget,
        "prepared secret fields retain only opaque status across IPC");
}

void TestChannel()
{
    DWORD handlesBefore = 0;
    // Creating the first USER message queue can lazily initialize shared
    // Windows/CRT resources. Compare equal, warmed process states.
    for (int iteration = -1; iteration < 12; ++iteration)
    {
        if (iteration == 0) GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
        if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) ||
            !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
            throw std::runtime_error("test pipe creation failed");
        Channel parent;
        parent.Open(mainRead, mainWrite, CurrentProcessHandle());
        parent.Bind<int, int>("host.preview", [](int value) { return value + 1; });
        std::promise<HWND> started;
        std::atomic<bool> childFailed = false;
        std::thread child([&] {
            try
            {
                Channel ui;
                ui.Open(uiRead, uiWrite, CurrentProcessHandle());
                ui.Bind<int, int>("ui.flush", [&ui](int value) {
                    return ui.Call<int>("host.preview", value) + 1;
                });
                ui.Bind<std::string, std::string>("ui.snapshot", [](std::string value) { return value; });
                ui.Bind<void>("ui.close", [] { PostQuitMessage(0); });
                started.set_value(ui.Window());
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
            }
            catch (...) { childFailed = true; try { started.set_value(nullptr); } catch (...) {} }
        });
        Check(started.get_future().get() != nullptr, "settings endpoint starts");
        try
        {
            Check(parent.Call<int>("ui.flush", 40) == 42,
                "nested host callback completes without deadlocking owner STAs");
            const std::string large(2 * 1024 * 1024, 'x');
            Check(parent.Call<std::string>("ui.snapshot", large) == large,
                "snapshots larger than pipe buffer arrive intact");
            parent.Notify("ui.close");
            child.join();
            Reject([&] { (void)parent.Request("ui.flush", Pack(1), 1000); },
                "closed child cannot acknowledge an unsaved edit");
            parent.Close();
            bool finalized = false;
            Check(parent.Post([&] { finalized = true; }),
                "host completion queue survives settings disconnect");
            MSG message{};
            while (PeekMessageW(&message, parent.Window(), 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
            Check(finalized, "durable backend completion runs after UI connection closes");
        }
        catch (const std::exception& error)
        {
            std::cerr << "IPC session iteration " << iteration << ": ";
            Check(false, error.what());
            PostThreadMessageW(GetThreadId(child.native_handle()), WM_QUIT, 0, 0);
            if (child.joinable()) child.join();
        }
        Check(!childFailed, "settings peer remained healthy");
    }
    DWORD handlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    if (handlesAfter > handlesBefore)
        std::cerr << "IPC handles: " << handlesBefore << " -> " << handlesAfter << '\n';
    Check(handlesAfter <= handlesBefore, "repeated IPC sessions release pipes, threads, events and process handles");
}

void TestStalledPeer()
{
    HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
    if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) ||
        !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
        throw std::runtime_error("test pipe creation failed");
    const auto started = GetTickCount64();
    {
        Channel channel;
        channel.Open(mainRead, mainWrite, CurrentProcessHandle());
        // No peer reader: this exceeds pipe capacity and stalls its writer.
        Reject([&] { (void)channel.Request("ui.flush", Pack(std::string(1024 * 1024, 'x')), 100); },
            "unresponsive peer fails instead of acknowledging an edit");
    }
    Check(GetTickCount64() - started < 3000,
        "a stalled pipe writer cannot block timeout or endpoint destruction");
    CloseHandle(uiWrite);
    CloseHandle(uiRead);
}

void TestProcessLifecycle()
{
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        Channel channel;
        SettingsProcess process;
        process.Start(channel);
        Check(process.Running() && process.ProcessId() != GetCurrentProcessId(),
            "settings UI runs in a distinct supervised process");
        Check(channel.Call<bool>("test.identity", ExecutableIdentity()),
            "child validates inherited channel and exact executable identity");
        snowdesktop::SettingsRoute route = snowdesktop::SettingsRoute::ForWidget(L"组件-42");
        Check(channel.Call<snowdesktop::SettingsRoute>("test.route", route) == route,
            "real child receives component route without sharing pointers");
        channel.Notify("test.exit");
        const auto deadline = GetTickCount64() + 5000;
        while (process.Running() && GetTickCount64() < deadline) Sleep(10);
        Check(!process.Running(), "settings child exits after close, without a resident UI process");
        channel.Close();
        process.Stop();
    }
    Channel channel;
    SettingsProcess process;
    process.Start(channel);
    Check(channel.Call<bool>("test.identity", ExecutableIdentity()),
        "supervised child starts before application shutdown simulation");
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, process.ProcessId());
    process.Stop();
    Check(child && WaitForSingleObject(child, 5000) == WAIT_OBJECT_0,
        "closing the application-owned job terminates the settings UI without an orphan process");
    if (child) CloseHandle(child);
    channel.Close();
}
}

int RunSettingsIpcChildIfRequested()
{
    using namespace snowdesktop::settings_ipc;
    if (!IsSettingsProcessCommand()) return -1;
    try
    {
        Channel channel;
        OpenInheritedSettingsChannel(channel);
        channel.Bind<bool, std::string>("test.identity", [](const std::string& identity) {
            return identity == ExecutableIdentity();
        });
        channel.Bind<snowdesktop::SettingsRoute, snowdesktop::SettingsRoute>("test.route", [](auto route) { return route; });
        channel.Bind<void>("test.exit", [] { PostQuitMessage(0); });
        channel.SetDisconnected([] { PostQuitMessage(ERROR_BROKEN_PIPE); });
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        return 0;
    }
    catch (...) { return 1; }
}

int RunSettingsIpcTests()
{
    TestCodec();
    TestChannel();
    TestStalledPeer();
    TestProcessLifecycle();
    return failures;
}
