#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/control/control_protocol.h"
#include "../test_temporary_directory.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace
{
using namespace snowdesktop::control;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::wstring Quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Case(const std::filesystem::path& script, bool errorReply, bool mismatch, bool oversized)
{
    snowdesktop::test::TemporaryDirectory temporary;
    wchar_t executable[MAX_PATH]{}, system[MAX_PATH]{};
    Check(GetModuleFileNameW(nullptr, executable, MAX_PATH) != 0, "test executable identity");
    Check(GetSystemDirectoryW(system, MAX_PATH) != 0, "Windows PowerShell location");
    const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    const auto parameters = temporary.path / L"parameters.json";
    std::ofstream(parameters, std::ios::binary) << R"({"label":"日程 — Kalender"})";
    const auto name = L"SnowDesktop.Control.Test." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64());
    const auto endpoint = L"\\\\.\\pipe\\" + name;
    const HANDLE pipe = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    Check(pipe != INVALID_HANDLE_VALUE, "isolated local fixture pipe");
    std::atomic<bool> received{false}, acknowledged{false}, valid{false};
    std::thread server([&] {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        const auto read = [&](void* bytes, DWORD count) {
            auto* cursor = static_cast<char*>(bytes);
            while (count)
            {
                DWORD transferred = 0;
                if (!ReadFile(pipe, cursor, count, &transferred, nullptr) || !transferred) return false;
                received.store(true); count -= transferred; cursor += transferred;
            }
            return true;
        };
        DWORD length = 0;
        if (!read(&length, sizeof(length)) || !length || length > 1048576) return;
        std::string request(length, '\0');
        if (!read(request.data(), length)) return;
        Value parsed;
        if (!ParseJson(request, parsed)) return;
        const auto* params = parsed.Find("params");
        const auto* id = parsed.Find("id");
        valid.store(id && params && params->Find("label") &&
            params->Find("label")->string == "日程 — Kalender" &&
            parsed.Find("method") && parsed.Find("method")->string == "app.getState");
        if (!id) return;
        const auto reply = Serialize(Object({{"jsonrpc", String("2.0")}, {"id", *id},
            {errorReply ? "error" : "result", errorReply
                ? Object({{"code", Number(-32000)}, {"message", String("fixture failure")},
                    {"data", Object({{"code", String("busy")}})}})
                : Object({{"label", String("日程 — Kalender")}})}}));
        length = oversized ? 1048577 : static_cast<DWORD>(reply.size());
        DWORD written = 0;
        if (!WriteFile(pipe, &length, sizeof(length), &written, nullptr)) return;
        if (!oversized && !WriteFile(pipe, reply.data(), length, &written, nullptr)) return;
        unsigned char ack = 0;
        acknowledged.store(read(&ack, 1) && ack == 6);
    });
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto stdoutPath = temporary.path / L"stdout.json";
    const HANDLE output = CreateFileW(stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE errors = CreateFileW((temporary.path / L"stderr.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output; startup.hStdError = errors;
    startup.hStdInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION process{};
    auto command = Quote(powershell) + L" -NoProfile -ExecutionPolicy Bypass -File " + Quote(script) +
        L" -Method app.getState -ParametersFile " + Quote(parameters) + L" -PipeName " + name +
        L" -ExecutablePath " + Quote(mismatch ? powershell : std::filesystem::path(executable));
    const bool started = CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    const auto wait = started ? WaitForSingleObject(process.hProcess, 20000) : WAIT_FAILED;
    DWORD exitCode = 999;
    if (started)
    {
        if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    CancelSynchronousIo(server.native_handle());
    server.join(); DisconnectNamedPipe(pipe); CloseHandle(pipe);
    CloseHandle(output); CloseHandle(errors); CloseHandle(startup.hStdInput);
    const auto diagnostic = Read(temporary.path / L"stderr.txt");
    if (!diagnostic.empty() && !mismatch && !oversized) std::cerr << diagnostic << '\n';
    Check(started && wait == WAIT_OBJECT_0, "PowerShell client terminates within the test deadline");
    Check(exitCode == (errorReply || mismatch || oversized ? 1u : 0u), "client reports success and failures accurately");
    if (mismatch)
    {
        Check(!received.load() && diagnostic.find("Unexpected IPC server") != std::string::npos,
            "wrong executable identity is rejected before sending any request");
        return;
    }
    Check(valid.load(), "real client frames and serializes named Unicode parameters");
    if (oversized)
    {
        Check(!acknowledged.load() && diagnostic.find("response length") != std::string::npos,
            "oversized response rejected without allocation or acknowledgement");
        return;
    }
    Check(acknowledged.load(), "client acknowledges the complete response before disconnecting");
    Value reply;
    Check(ParseJson(Read(stdoutPath), reply) && reply.Find(errorReply ? "error" : "result"),
        "stdout preserves a parseable JSON-RPC success or domain-error envelope");
    if (!errorReply)
        Check(reply.Find("result")->Find("label")->string == "日程 — Kalender", "Unicode result survives the full pipe round trip");
}
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Check(argc == 2, "client script path required");
        Case(argv[1], false, false, false);
        Case(argv[1], true, false, false);
        Case(argv[1], false, true, false);
        Case(argv[1], false, false, true);
        std::cout << "Control Skill client transport checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
