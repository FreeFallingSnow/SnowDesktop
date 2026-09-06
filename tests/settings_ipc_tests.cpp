#include "settings_ipc_channel.h"

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
}

void TestChannel()
{
    DWORD handlesBefore = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    for (int iteration = 0; iteration < 12; ++iteration)
    {
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
            Check(false, error.what());
            PostThreadMessageW(GetThreadId(child.native_handle()), WM_QUIT, 0, 0);
            if (child.joinable()) child.join();
        }
        Check(!childFailed, "settings peer remained healthy");
    }
    DWORD handlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    Check(handlesAfter <= handlesBefore + 2, "repeated IPC sessions release pipes, threads, events and process handles");
}
}

int RunSettingsIpcTests()
{
    TestCodec();
    TestChannel();
    return failures;
}
