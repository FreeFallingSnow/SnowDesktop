#include "shell/file_preview.h"

#include <objbase.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace preview = snowdesktop::file_preview;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
        Require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(2ms);
    }
}
std::string Utf8(std::wstring_view text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}
void Write(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    Require(static_cast<bool>(stream), "write isolated provider fixture");
}
std::string Read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    Require(static_cast<bool>(stream), "read actual fixture output");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Temp
{
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path()), root;
    Temp()
    {
        GUID id{}; Require(SUCCEEDED(CoCreateGuid(&id)), "create provider fixture identity");
        wchar_t text[40]{}; StringFromGUID2(id, text, 40);
        root = parent / (std::wstring(L"SnowDesktopPreview-") + text);
        fs::create_directories(root);
    }
    ~Temp()
    {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename().wstring().starts_with(L"SnowDesktopPreview-")) fs::remove_all(resolved, error);
    }
};
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Pipe
{
    std::wstring name;
    Handle server, connected;
    OVERLAPPED connection{};
    explicit Pipe(std::wstring suffix)
    {
        name = L"\\\\.\\pipe\\SnowDesktop.FilePreview.Tests." + suffix;
        server.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        Require(server.value != INVALID_HANDLE_VALUE, "create an isolated real provider pipe");
        connected.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Require(connected.value != nullptr, "create provider connection event");
        connection.hEvent = connected.value;
        Require(!ConnectNamedPipe(server.value, &connection) && GetLastError() == ERROR_IO_PENDING,
            "listen without connecting to the real QuickLook service");
    }
    ~Pipe()
    {
        CancelIoEx(server.value, nullptr);
        DWORD ignored = 0;
        GetOverlappedResult(server.value, &connection, &ignored, TRUE);
    }
    std::string Receive()
    {
        Require(WaitForSingleObject(connected.value, 2000) == WAIT_OBJECT_0, "receive actual provider connection");
        Handle readEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        OVERLAPPED operation{}; operation.hEvent = readEvent.value;
        std::string text(65536, '\0'); DWORD bytes = 0;
        if (!ReadFile(server.value, text.data(), static_cast<DWORD>(text.size()), &bytes, &operation))
        {
            Require(GetLastError() == ERROR_IO_PENDING, "read actual provider bytes");
            Require(WaitForSingleObject(readEvent.value, 2000) == WAIT_OBJECT_0 &&
                GetOverlappedResult(server.value, &operation, &bytes, FALSE), "complete real pipe read");
        }
        text.resize(bytes); return text;
    }
};

const std::string enabled = R"({"enabled":{"Peek":true}})";
const std::string space = R"({"properties":{"EnableSpaceToActivate":{"value":true}}})";
const std::string controlSpace = R"({"properties":{"EnableSpaceToActivate":{"value":false},"ActivationShortcut":{"ctrl":true,"alt":false,"shift":false,"win":false,"code":32}}})";
const std::string altP = R"({"properties":{"EnableSpaceToActivate":{"value":false},"ActivationShortcut":{"ctrl":false,"alt":true,"shift":false,"win":false,"code":80}}})";

void CheckSettingsAndRouting()
{
    Require(preview::ParsePeekShortcut(enabled, space) == preview::Shortcut{}, "follow Peek's bare Space mode");
    Require(preview::ParsePeekShortcut(enabled, controlSpace) == preview::Shortcut{VK_SPACE, MOD_CONTROL},
        "follow the existing Control+Space binding");
    Require(preview::ParsePeekShortcut(enabled, altP) == preview::Shortcut{'P', MOD_ALT},
        "follow customized Alt shortcuts without registering another global hotkey");
    Require(!preview::ParsePeekShortcut(R"({"enabled":{"Peek":false}})", space), "disabled Peek stays disabled");
    Require(!preview::ParsePeekShortcut("{}", space), "installation alone cannot enable Peek");
    Require(!preview::ParsePeekShortcut(enabled, "{}"), "unknown config cannot invent a shortcut");
    Require(!preview::ParsePeekShortcut("broken", space), "partial settings writes fail closed");
    Require(!preview::ParsePeekShortcut(enabled,
        R"({"properties":{"ActivationShortcut":{"ctrl":false,"alt":false,"shift":false,"win":false,"code":32.5}}})"),
        "invalid fractional key codes cannot be truncated into real shortcuts");
    Require(preview::ParsePeekShortcut(enabled,
        R"({"properties":{"EnableSpaceToActivate":{"value":false},"ActivationShortcut":{"ctrl":false,"alt":false,"shift":false,"win":false,"code":32}}})") == preview::Shortcut{VK_SPACE, MOD_CONTROL},
        "mirror Peek's fallback after bare Space mode is disabled");
    for (const auto context : {preview::InputContext{true}, preview::InputContext{false, true},
            preview::InputContext{false, false, true}})
        Require(!preview::AllowsPreview(context), "input, overlays and gestures retain their key ownership");
    Require(preview::AllowsPreview({}), "ordinary file selection can preview");
    preview::Availability available{{{preview::Kind::Peek, {}, L"peek.exe"},
        {preview::Kind::QuickLook, {}, L"pipe"}}};
    Require(preview::MatchProvider(available, {})->kind == preview::Kind::QuickLook,
        "one physical Space chooses QuickLook even when enumeration order changes");
    Require(!preview::MatchProvider(available, {VK_SPACE, MOD_SHIFT}), "extra modifiers must not invoke either tool");
    for (const auto* path : {L"relative.pdf", L"https://example.com/test", L"::{virtual}",
            L"C:\\file.pdf|Close", L"C:\\file.pdf\ncommand"})
        Require(!preview::IsFileSystemPath(path), "reject non-filesystem and protocol-injection targets");
}

void CheckDiscoveryAndPipe(Temp& temp)
{
    preview::Environment environment;
    environment.powerToysRunning = true;
    environment.powerToysSettings = temp.root / L"settings";
    const auto executable = temp.root / L"provider/Peek.exe";
    Write(executable, "isolated executable metadata boundary");
    environment.peekInstallations.push_back({executable, 0, 98});
    Write(environment.powerToysSettings / L"settings.json", enabled);
    Write(environment.powerToysSettings / L"Peek/settings.json", controlSpace);
    Require(preview::Probe(environment).providers.size() == 1, "read enabled Peek from real isolated settings files");
    Write(environment.powerToysSettings / L"Peek/settings.json", altP);
    Require(preview::Probe(environment).providers.front().shortcut == preview::Shortcut{'P', MOD_ALT},
        "provider discovery follows a real external configuration change");
    environment.powerToysRunning = false;
    Require(preview::Probe(environment).providers.empty(), "installed but stopped PowerToys cannot claim keys");
    environment.powerToysRunning = true;
    environment.peekInstallations.front().minor = 95;
    Require(preview::Probe(environment).providers.empty(), "Peek without the CLI capability is not advertised");
    environment.peekInstallations.front().minor = 98;
    Write(environment.powerToysSettings / L"settings.json", R"({"enabled":{"Peek":false}})");
    Require(preview::Probe(environment).providers.empty(), "a disabled provider cannot be revived by its executable");

    Pipe pipe(temp.root.filename().wstring());
    environment.quickLookPipe = pipe.name;
    const auto detected = preview::Probe(environment);
    Require(detected.providers.size() == 1 && detected.providers[0].kind == preview::Kind::QuickLook,
        "detect a listening QuickLook-compatible endpoint without requiring its install path");
    Require(WaitForSingleObject(pipe.connected.value, 0) == WAIT_TIMEOUT,
        "availability probing must not consume a connection or deliver an empty line");
    const auto document = temp.root / L"space 中文/selected document.txt";
    Write(document, "preview, never execute the document");
    preview::ProcessIdentity session;
    Require(preview::Invoke(detected.providers.front(), document.wstring(),
            [&](auto process) { session = process; }), "send the selected file through the production pipe writer");
    Require(session == preview::IdentifyProcess(GetCurrentProcessId()), "bind selection retention to the real pipe server process");
    Require(pipe.Receive() == "QuickLook.App.PipeMessages.Toggle|" + Utf8(document.wstring()) + "|\n",
        "the real UTF-8 wire message carries the exact selected path, including spaces and Chinese");
    Require(!preview::Invoke(detected.providers.front(), (temp.root / L"missing.txt").wstring(), [](auto) {}),
        "deleted targets never produce another provider request");
}

void CheckActualPeekLaunch(Temp& temp, const fs::path& self)
{
    const auto executable = temp.root / L"program with spaces/preview-launch-fixture.exe";
    fs::create_directories(executable.parent_path());
    fs::copy_file(self, executable);
    const auto document = temp.root / L"argument 中文/trailing directory";
    fs::create_directories(document);
    const auto argument = document.wstring() + L"\\";
    Handle process;
    Require(preview::Invoke({preview::Kind::Peek, {}, executable.wstring()}, argument,
        [&](auto identity) {
            Require(identity.pid && identity.created, "identify the actual suspended preview child before activation");
            process.value = OpenProcess(SYNCHRONIZE, FALSE, identity.pid);
        }), "launch the actual CLI fixture through production CreateProcess");
    Require(process.value && WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0,
        "wait for the owned headless CLI fixture to finish");
    Require(Read(executable.parent_path() / L"received-path.txt") == Utf8(argument),
        "the real Windows argv decoder receives exactly one quoted Unicode path with its trailing slash");
}

void CheckService(Temp& temp)
{
    struct Observed
    {
        std::mutex mutex;
        preview::Availability available{{{preview::Kind::QuickLook, {}, L"isolated-pipe"}}};
        std::vector<std::wstring> invoked;
        std::atomic<unsigned> detections{0};
    };
    const auto observed = std::make_shared<Observed>();
    preview::Service service({
        [observed] {
            std::lock_guard lock(observed->mutex); ++observed->detections;
            return observed->available;
        },
        [observed](const auto&, const std::wstring& path, const preview::PublishSession& publish) {
            publish(preview::IdentifyProcess(GetCurrentProcessId()));
            std::lock_guard lock(observed->mutex); observed->invoked.push_back(path); return true;
        }});
    service.Start();
    Await([&] { return service.Matches({}); }, "publish initial provider detection");
    const auto selected = (temp.root / L"selected.txt").wstring();
    Require(service.Request({}, selected, false), "a selected file queues one preview");
    for (unsigned repeat = 0; repeat < 8; ++repeat)
        Require(service.Request({}, selected, true), "consume repeats without queuing additional toggles");
    Await([&] { std::lock_guard lock(observed->mutex); return observed->invoked.size() == 1; }, "deliver the copied file selection");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"isolated preview identity", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(owner != nullptr, "create a message-only focus identity");
    const bool retained = service.RetainsSelection(owner);
    DestroyWindow(owner);
    Require(retained, "only the successfully published preview process retains selection");
    Require(!service.RetainsSelection(nullptr), "an unrelated/unknown focus transfer ends the lease");

    {
        std::lock_guard lock(observed->mutex); observed->available = {};
    }
    Require(service.Request({}, selected, false), "exercise an obsolete UI availability cache");
    Await([&] { return service.Current().providers.empty(); }, "recheck provider availability before dispatch");
    {
        std::lock_guard lock(observed->mutex);
        Require(observed->invoked.size() == 1 && observed->invoked.front() == selected,
            "disable-before-dispatch and repeated input cannot invoke the provider again");
        observed->available.providers = {{preview::Kind::Peek, {'P', MOD_ALT}, L"new-provider"}};
    }
    service.Refresh();
    Await([&] { return service.Matches({'P', MOD_ALT}); }, "publish changed provider shortcut");
    Require(!service.Matches({}), "the old Space binding is released after a settings change");
    service.Stop();
    Require(!service.Request({'P', MOD_ALT}, selected, false) && service.Current().providers.empty(),
        "shutdown releases keys and rejects pending launches");
}

void CheckStopDuringDetection()
{
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false;
        std::atomic<bool> finished{false}, invoked{false};
    };
    const auto gate = std::make_shared<Gate>();
    preview::Service service({[gate] {
        std::unique_lock lock(gate->mutex);
        gate->entered = true; gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
        gate->finished = true;
        return preview::Availability{{{preview::Kind::QuickLook, {}, L"late-provider"}}};
    }, [gate](const auto&, const auto&, const auto&) { gate->invoked = true; return true; }});
    service.Start();
    {
        std::unique_lock lock(gate->mutex);
        Require(gate->changed.wait_for(lock, 5s, [&] { return gate->entered; }), "enter isolated blocked detection boundary");
    }
    // Stop must return while the OS boundary is still blocked; no host pointer
    // is retained and a late detection result cannot enable keys or launch UI.
    service.Stop();
    {
        std::lock_guard lock(gate->mutex); gate->release = true; gate->changed.notify_all();
    }
    Await([&] { return gate->finished.load(); }, "release the isolated detection worker");
    Require(service.Current().providers.empty() && !gate->invoked,
        "shutdown rejects a provider result that finishes after the host has stopped");
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        const fs::path self = fs::absolute(argv[0]);
        if (self.filename() == L"preview-launch-fixture.exe")
        {
            Require(argc == 2, "CLI fixture must receive exactly one file argument");
            Write(self.parent_path() / L"received-path.txt", Utf8(argv[1]));
            return 0;
        }
        Temp temp;
        CheckSettingsAndRouting();
        CheckDiscoveryAndPipe(temp);
        CheckActualPeekLaunch(temp, self);
        CheckService(temp);
        CheckStopDuringDetection();
        std::cout << "File preview settings, discovery, real pipe/argv and asynchronous routing passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
