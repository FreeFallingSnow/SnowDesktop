#include "platform/control/control_protocol.h"
#include "platform/control/control_server.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using namespace snowdesktop::control;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
Value Parse(std::string_view text)
{
    Value value; Check(ParseJson(text, value), "response must be JSON"); return value;
}
std::string ErrorCode(const Value& value)
{
    const auto* error = value.Find("error");
    return error ? error->Find("data")->Find("code")->string : "";
}
void Protocol()
{
    int mutations = 0;
    const Handler handler = [&](std::string_view, const Value& params) {
        Fields(params, {}); ++mutations;
        return Object({{"visible", Boolean(true)}, {"label", String("日程 — Kalender")}});
    };
    const auto invoke = [&](std::string_view input) { return Parse(Dispatch(input, handler)); };
    const auto valid = invoke(R"({"jsonrpc":"2.0","id":"sample","method":"app.getState"})");
    Check(valid.Find("id")->string == "sample" && valid.Find("result")->Find("visible")->boolean,
        "structured results and request correlation survive dispatch");
    Check(valid.Find("result")->Find("label")->string == "日程 — Kalender", "UTF-8 survives round trip");
    Check(mutations == 1, "valid call executes exactly once");
    Check(ErrorCode(invoke("{")) == "parse_error", "malformed JSON rejected");
    Check(ErrorCode(invoke(R"([])")) == "invalid_request", "batches rejected");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","method":"navigation.open"})")) == "invalid_request", "notifications rejected");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","id":1.5,"method":"navigation.open"})")) == "invalid_request", "fractional IDs rejected");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","id":9007199254740992,"method":"navigation.open"})")) == "invalid_request", "unsafe numeric IDs rejected");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","id":1,"method":"arbitrary.shell"})")) == "method_not_found", "unknown operations never dispatch");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","id":1,"method":"app.getState","params":[]})")) == "invalid_params", "array params rejected");
    Check(ErrorCode(invoke(R"({"jsonrpc":"2.0","id":1,"method":"app.getState","params":{"unexpected":true}})")) == "invalid_params", "unknown fields rejected before mutation");
    Check(mutations == 1, "invalid requests must not mutate backend");
    const auto discovery = invoke(R"({"jsonrpc":"2.0","id":9,"method":"api.describe"})");
    Check(discovery.Find("result")->Find("methods")->array.size() >= 10, "discovery lists supported methods");
    const auto* capabilities = discovery.Find("result")->Find("capabilities");
    Check(capabilities && capabilities->Find("elements")->number == 1 &&
        capabilities->Find("controlledPointer")->number == 1 && capabilities->Find("demoCursor")->number == 1,
        "clients can negotiate element and pointer capabilities without changing protocol v1");
    Check(capabilities->Find("monitorCapture") && capabilities->Find("monitorCapture")->number == 1,
        "clients can discover the optional monitor-capture extension");
    Check(capabilities->Find("menus") && capabilities->Find("menus")->number == 1,
        "clients can discover visible modern-menu controls");
    Check(capabilities->Find("settingsAutomation") && capabilities->Find("settingsAutomation")->number == 1,
        "clients can negotiate the asynchronous settings adapter");
    Check(capabilities->Find("demoRender") && capabilities->Find("demoRender")->number == 1,
        "clients can negotiate isolated native scene exports");
    Check(capabilities->Find("nativeWidgetControls")->number == 1 && capabilities->Find("demoElementTargets")->number == 1,
        "clients can negotiate native control geometry and scene cursor targets");
    Check(mutations == 1, "discovery is independent of UI mutations");
    const auto failure = Parse(Dispatch(R"({"jsonrpc":"2.0","id":"old","method":"widgets.place"})",
        [](std::string_view, const Value&) -> Value { throw Error("stale_layout", "refresh required"); }));
    Check(ErrorCode(failure) == "stale_layout" && failure.Find("id")->string == "old", "domain failures preserve code and correlation");
    Value parameters = Parse(R"({"column":1.5,"id":""})");
    bool rejected = false;
    try { (void)Integer(parameters, "column", 0, 10); } catch (const Error&) { rejected = true; }
    Check(rejected, "grid coordinates cannot be fractional");
    rejected = false;
    try { (void)Text(parameters, "id"); } catch (const Error&) { rejected = true; }
    Check(rejected, "stable target ID must be nonempty");
}

HANDLE Connect(const std::wstring& endpoint)
{
    const auto deadline = GetTickCount64() + 3000;
    do
    {
        HANDLE pipe = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) break;
        WaitNamedPipeW(endpoint.c_str(), 20);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("test client cannot connect");
}
bool Transfer(HANDLE pipe, void* bytes, DWORD length, bool write)
{
    DWORD done = 0;
    while (done < length)
    {
        DWORD transferred = 0;
        auto* position = static_cast<unsigned char*>(bytes) + done;
        const bool ok = (write ? WriteFile(pipe, position, length - done, &transferred, nullptr) :
            ReadFile(pipe, position, length - done, &transferred, nullptr)) != FALSE;
        if (!ok || !transferred) return false;
        done += transferred;
    }
    return true;
}
std::string Call(const std::wstring& endpoint, std::string request, HANDLE written = nullptr)
{
    HANDLE pipe = Connect(endpoint);
    DWORD length = static_cast<DWORD>(request.size());
    bool ok = Transfer(pipe, &length, sizeof(length), true) && Transfer(pipe, request.data(), length, true);
    if (written) SetEvent(written);
    if (ok) ok = Transfer(pipe, &length, sizeof(length), false) && length <= MaximumFrameBytes;
    std::string response(ok ? length : 0, '\0');
    if (ok) ok = Transfer(pipe, response.data(), length, false);
    unsigned char acknowledgement = 6;
    if (ok) ok = Transfer(pipe, &acknowledgement, 1, true);
    CloseHandle(pipe);
    return ok ? response : "";
}
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
std::string Complete(std::future<std::string>& future)
{
    const auto deadline = GetTickCount64() + 5000;
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && GetTickCount64() < deadline)
    {
        Pump();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    Check(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, "client completes within deadline");
    return future.get();
}

void Transport()
{
    const auto endpoint = EndpointName() + L".test." + std::to_wstring(GetCurrentProcessId());
    Check(!EndpointName().empty(), "endpoint resolves current interactive user/session");
    Server server;
    int mutations = 0;
    const DWORD owner = GetCurrentThreadId();
    Check(server.Start([&](std::string_view method, const Value& params) {
        Check(GetCurrentThreadId() == owner, "operations execute on owning UI thread");
        Fields(params, {});
        ++mutations;
        return Object({{"method", String(std::string(method))}, {"sequence", Number(mutations)}});
    }, endpoint, 1000), "server starts on private fixture endpoint");
    Server duplicate;
    Check(!duplicate.Start([](std::string_view, const Value&) { return Object(); }, endpoint),
        "second owner cannot replace an existing pipe");
    auto invoke = [&] {
        return std::async(std::launch::async, [&] {
            return Call(endpoint, R"({"jsonrpc":"2.0","id":"live","method":"navigation.open","params":{}})");
        });
    };
    auto client = invoke();
    auto response = Parse(Complete(client));
    Check(response.Find("result")->Find("sequence")->number == 1, "real pipe dispatches and returns structured result");
    Check(response.Find("id")->string == "live", "wire preserves request ID");
    // Deliberately do not pump the UI. The worker's queued timeout must cancel
    // the operation; delivering its stale window message cannot mutate later.
    auto expired = invoke();
    Check(expired.wait_for(std::chrono::seconds(4)) == std::future_status::ready, "unserviced UI has a bounded deadline");
    Check(expired.get().empty(), "expired request has no success reply");
    Pump();
    Check(mutations == 1, "timed-out queued action cannot execute after timeout");
    auto recovered = invoke();
    response = Parse(Complete(recovered));
    Check(response.Find("result")->Find("sequence")->number == 2, "connection recovers after timeout");
    auto oversized = std::async(std::launch::async, [&] {
        HANDLE pipe = Connect(endpoint);
        DWORD length = static_cast<DWORD>(MaximumFrameBytes + 1);
        Check(Transfer(pipe, &length, sizeof(length), true), "send oversized frame header");
        unsigned char byte = 0;
        const bool accepted = Transfer(pipe, &byte, 1, false);
        CloseHandle(pipe);
        return std::string(accepted ? "accepted" : "rejected");
    });
    Check(Complete(oversized) == "rejected", "oversized frame rejected without UI dispatch");
    Check(mutations == 2, "oversized frame cannot mutate");
    auto invalidUtf8 = std::async(std::launch::async, [&] {
        return Call(endpoint, std::string("{\"x\":\"") + static_cast<char>(0xff) + "\"}");
    });
    Check(Complete(invalidUtf8).empty(), "invalid UTF-8 is rejected at the transport boundary");
    HANDLE written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(written != nullptr, "create shutdown fixture event");
    auto pending = std::async(std::launch::async, [&] {
        return Call(endpoint, R"({"jsonrpc":"2.0","id":3,"method":"navigation.open"})", written);
    });
    // The client is connected and has written a request. Do not pump the UI.
    const bool sent = WaitForSingleObject(written, 3000) == WAIT_OBJECT_0;
    CloseHandle(written);
    Check(sent, "client writes request before shutdown");
    server.Stop();
    Check(pending.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "shutdown releases waiting client");
    Check(pending.get().empty(), "shutdown cannot acknowledge unexecuted work");
    Pump();
    Check(mutations == 2 && !server.Running(), "shutdown cancels queued work and removes endpoint");
}
}

void ApplicationControlElementChecks();
int main()
{
    try { Protocol(); ApplicationControlElementChecks(); Transport(); std::cout << "Application control protocol, element, cursor and real-pipe checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
