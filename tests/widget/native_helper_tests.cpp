#include "widget/tasks/widget_native_task_executor.h"
#include "widget/packages/widget_package.h"
#include "common/json_value.h"
#include "test_temporary_directory.h"

#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace
{
using namespace snowdesktop::widget;
using namespace snowdesktop::widget_runtime;
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    Require(static_cast<bool>(stream), "fixture write succeeds");
}
int Helper()
{
    std::string line;
    if (!std::getline(std::cin, line)) return 1;
    JsonValue hello;
    if (!ParseJson(line, hello) || !hello.Find("protocol")) return 2;
    std::cout << R"({"version":1,"kind":"hello","protocol":"snowdesktop-native"})" << std::endl;
    if (!std::getline(std::cin, line)) return 3;
    JsonValue request;
    if (!ParseJson(line, request) || !request.Find("operation") || !request.Find("payload")) return 4;
    const auto operation = request.Find("operation")->string;
    if (operation == "watch-silent") { std::getline(std::cin, line); return 0; }
    if (operation == "watch" || operation == "watch-crash" || operation == "watch-burst")
    {
        unsigned sequence = 0;
        for (;;)
        {
            std::cout << "{\"version\":1,\"kind\":\"event\",\"id\":\"1\",\"value\":{\"pid\":" <<
                GetCurrentProcessId() << ",\"sequence\":" << ++sequence << ",\"percent\":73}}" << std::endl;
            if (operation == "watch-crash") return 12;
            DWORD available = 0;
            if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr)) return 13;
            if (available) { std::getline(std::cin, line); return 0; }
            if (operation != "watch-burst") Sleep(80);
        }
    }
    if (operation == "bad")
        std::cout << R"({"version":9,"kind":"result","id":"1","value":false})" << std::endl;
    else if (operation == "bad-json")
        std::cout << "debug text containing a private token" << std::endl;
    else if (operation == "gbk")
        std::cout << "{\"version\":1,\"kind\":\"result\",\"id\":\"1\",\"value\":\"" <<
            "\xD6\xD0" << "\"}" << std::endl;
    else if (operation == "large")
        std::cout << std::string(20000, 'x') << std::endl;
    else
    {
        std::cout << "{\"version\":1,\"kind\":\"event\",\"id\":\"1\",\"value\":{\"pid\":" << GetCurrentProcessId() << ",\"percent\":0}}" << std::endl;
        if (operation == "hang")
        { std::getline(std::cin, line); return 0; }
        if (operation == "failure")
            std::cout << R"({"version":1,"kind":"error","id":"1","code":"deviceUnavailable"})" << std::endl;
        else
            std::cout << R"({"version":1,"kind":"result","id":"1","value":{"percent":73,"charging":false}})" << std::endl;
    }
    std::getline(std::cin, line);
    return 0;
}
struct Runner
{
    std::mutex mutex;
    std::condition_variable ready;
    unsigned revision = 0;
    WidgetNativeTaskExecutor executor;
    Runner()
    {
        executor.SetCompletionCallback([this] {
            std::lock_guard lock(mutex);
            ++revision;
            ready.notify_one();
        });
    }
    std::vector<NativeTaskMessage> Wait(bool untilFinal = true)
    {
        std::vector<NativeTaskMessage> output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        unsigned seen = 0;
        for (;;)
        {
            // Do not hold the callback mutex while acquiring executor state.
            auto messages = executor.DrainMessages();
            bool finished = false;
            for (auto& message : messages)
            {
                finished = finished || message.final || !untilFinal;
                output.push_back(std::move(message));
            }
            if (finished) return output;
            std::unique_lock lock(mutex);
            Require(ready.wait_until(lock, deadline, [&] { return revision != seen; }), "native task completes within a bounded wait");
            seen = revision;
        }
    }
};

void ProbeManagedHelper(const std::filesystem::path& executable)
{
    snowdesktop::test::TemporaryDirectory temporary;
    const auto root = temporary.path / L"托管 helper 参数与编码";
    std::filesystem::create_directories(root);
    std::filesystem::copy_file(executable, root / L"helper.exe");
    const auto hash = WidgetPackageManager::Sha256File(root / L"helper.exe");
    Write(root / L"main.lua", "return widget.define({model={}})");
    Write(root / L"widget.json", std::string(R"({"schemaVersion":2,"apiVersion":2,"id":"4776c7c1-36a3-4c1e-86cc-9ab8193d9a76","slug":"managed-test","version":"1.0.0","name":"Managed test","entry":"main.lua","permissions":["native.execute"],"requiredFeatures":["task.native.request"],"nativeHelpers":{"battery":{"path":"helper.exe","sha256":")") + hash +
        R"(","purposeKey":"p","operationsKey":"p","reasonKey":"p","deniedKey":"p"}},"locales":{"en-US":{"p":"Managed test fixture"}}})");
    PackageManifest manifest;
    Require(WidgetPackageValidator{}.ValidateDirectory(root, &manifest).Ok(), "managed fixture validates");
    const auto fingerprint = PackagePermissionScopeFingerprint(manifest);
    {
        Runner runner;
        Require(runner.executor.Start(31, root, fingerprint, {"battery","read","{}",5000}), "managed helper starts through production executor");
        const auto messages = runner.Wait();
        JsonValue frame;
        Require(messages.back().ok && ParseJson(messages.back().json, frame), "managed UTF-8 result arrives");
        const auto* value = frame.Find("value");
        Require(value && value->Find("argc") && value->Find("argc")->number == 1 &&
            value->Find("arg") && value->Find("arg")->string == "--snowdesktop-native-v1" &&
            value->Find("label") && value->Find("label")->string == "鼠标 · 2.4G",
            ".NET Framework receives the fixed argument in a Unicode/spaced path and delivers explicit UTF-8 unchanged");
    }
    {
        Runner runner;
        Require(runner.executor.Start(32, root, fingerprint, {"battery","gbk","{}",5000}), "managed GBK negative control starts");
        const auto messages = runner.Wait();
        Require(messages.back().error == "nativeProtocolError" &&
            messages.back().diagnostic.find("invalidUtf8") != std::string::npos,
            "the same managed helper emitting CP936 bytes fails specifically on UTF-8 validation");
    }
}
void ProbeDemoPackage(const std::filesystem::path& package)
{
    // Exercise the actual authoring package without changing live user data.
    snowdesktop::test::TemporaryDirectory temporary;
    const auto root = temporary.path / L"真实演示组件 package";
    std::filesystem::copy(package, root, std::filesystem::copy_options::recursive);
    PackageManifest manifest;
    Require(WidgetPackageValidator{}.ValidateDirectory(root, &manifest).Ok(), "actual demo copy validates with its declared EXE hashes");
    Require(manifest.id == "4821cc55-f1dc-4322-9a1e-c697ebd4ace6", "probe uses the existing native EXE demo identity");
    const auto fingerprint = PackagePermissionScopeFingerprint(manifest);
    const auto processFrom = [](const NativeTaskMessage& message) {
        JsonValue frame;
        Require(!message.final && ParseJson(message.json, frame), "actual demo emits an event frame");
        const auto* value = frame.Find("value");
        Require(value && value->Find("pid") && value->Find("sequence") &&
            value->Find("percent") && value->Find("percent")->number == 73 &&
            value->Find("simulated") && value->Find("simulated")->boolean,
            "actual demo sends the simulated reading and process identity");
        return std::pair<DWORD, double>{static_cast<DWORD>(value->Find("pid")->number), value->Find("sequence")->number};
    };
    struct Process
    {
        HANDLE value = nullptr;
        explicit Process(DWORD id) : value(OpenProcess(SYNCHRONIZE, FALSE, id))
        { Require(value != nullptr, "actual demo process is observable"); }
        ~Process() { if (value) CloseHandle(value); }
    };
    {
        Runner runner;
        Require(runner.executor.Start(41, root, fingerprint, {"battery","watch","{}",5000,true}), "actual demo resident task starts");
        DWORD pid = 0;
        double sequence = -1;
        unsigned events = 0;
        while (events < 3)
            for (const auto& message : runner.Wait(false))
            {
                const auto [observedPid, observedSequence] = processFrom(message);
                Require((pid == 0 || pid == observedPid) && observedSequence > sequence,
                    "three actual demo updates keep the same process and advance sequence");
                pid = observedPid;
                sequence = observedSequence;
                ++events;
            }
        std::cout << "actual demo resident pid=" << pid << " updates=" << events << " lastSequence=" << sequence << '\n';
        Process process(pid);
        Require(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, "actual helper stays resident after multiple readings");
        runner.executor.Cancel(41);
        const auto messages = runner.Wait();
        Require(messages.back().final && messages.back().error == "canceled", "actual demo cancellation completes");
        Require(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "actual demo process exits after cancellation");
    }
    {
        Runner runner;
        Require(runner.executor.Start(42, root, fingerprint, {"battery","watch","{}",5000,true}), "actual demo mutation probe starts");
        const auto messages = runner.Wait(false);
        Process process(processFrom(messages.front()).first);
        Write(root / L"native-validation-marker.txt", "isolated package change");
        const auto stopped = runner.Wait();
        Require(stopped.back().error == "nativeContentChanged" &&
            stopped.back().diagnostic.find("packageChangeDetected") != std::string::npos &&
            stopped.back().diagnostic.find("native-validation-marker.txt") != std::string::npos,
            "actual demo package change reports its cause and relative path");
        Require(WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0, "package change retires the actual demo process");
    }
    std::filesystem::remove(root / L"native-validation-marker.txt");
    PackageManifest restored;
    Require(WidgetPackageValidator{}.ValidateDirectory(root, &restored).Ok() &&
        PackagePermissionScopeFingerprint(restored) == fingerprint,
        "the isolated package is restored to exactly its reviewed content before further launches");
    for (const auto& operation : {"read", "gbk", "bad-json"})
    {
        Runner runner;
        Require(runner.executor.Start(43, root, fingerprint, {"protocol",operation,"{}",5000}), "actual package protocol probe starts");
        const auto messages = runner.Wait();
        if (std::string_view(operation) == "read")
        {
            JsonValue frame;
            Require(messages.back().ok && ParseJson(messages.back().json, frame), "actual package UTF-8 probe succeeds");
            const auto* value = frame.Find("value");
            Require(value && value->Find("argc") && value->Find("argc")->number == 1 &&
                value->Find("arg") && value->Find("arg")->string == "--snowdesktop-native-v1" &&
                value->Find("label") && value->Find("label")->string == "鼠标 · 2.4G",
                "actual package probe receives fixed argv and preserves Chinese UTF-8");
        }
        else
        {
            const auto reason = std::string_view(operation) == "gbk" ? "invalidUtf8" : "invalidJson";
            Require(messages.back().error == "nativeProtocolError" &&
                messages.back().diagnostic.find(reason) != std::string::npos,
                "actual package malformed output is diagnosed correctly");
        }
    }
    std::cout << "actual native EXE demo: stable PID, increasing updates, cancel exit and package-change exit passed\n";
}
void Tests()
{
    snowdesktop::test::TemporaryDirectory temporary;
    const auto root = temporary.path / L"测试 helper package";
    std::filesystem::create_directories(root);
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    Require(length > 0 && length < 32768, "fixture finds its own executable");
    std::filesystem::copy_file(executable, root / L"helper.exe");
    const auto hash = WidgetPackageManager::Sha256File(root / L"helper.exe");
    const auto manifestText = [&] {
        return std::string(R"({"schemaVersion":2,"apiVersion":2,"id":"1c60540b-d29b-4938-916c-e998f830de40","slug":"native-test","version":"1.0.0","name":"Native test","entry":"main.lua","permissions":["native.execute"],"requiredFeatures":["task.native.request"],"nativeHelpers":{"battery":{"path":"helper.exe","sha256":")") + hash +
            R"(","purposeKey":"purpose","operationsKey":"operations","reasonKey":"reason","deniedKey":"denied"}},"locales":{"en-US":{"purpose":"Show charge","operations":"Read device status","reason":"Native device access","denied":"No data"},"ja-JP":{"purpose":"電量表示","operations":"デバイス状態の取得","reason":"デバイスへのアクセス","denied":"データなし"}}})";
    };
    Write(root / L"main.lua", "return widget.define({model={}})");
    Write(root / L"widget.json", manifestText());
    WidgetPackageValidator validator;
    PackageManifest manifest;
    auto report = validator.ValidateDirectory(root, &manifest);
    if (!report.Ok()) std::cerr << report.ToJson() << '\n';
    Require(report.Ok() && manifest.nativeHelpers.size() == 1, "declared EXE with complete localized explanation validates");
    Require(LocalizePackageManifest(manifest, "ja-JP").nativeHelpers.front().purpose == "電量表示", "native consent text uses component localization");
    const auto fingerprint = PackagePermissionScopeFingerprint(manifest);
    auto watchManifest = manifestText();
    watchManifest.replace(watchManifest.find("task.native.request"), std::string("task.native.request").size(), "task.native.watch");
    Write(root / L"widget.json", watchManifest);
    Require(validator.ValidateDirectory(root).Ok(), "watch-only required feature declares a valid native package");
    Write(root / L"widget.json", manifestText());

    Write(root / L"undeclared.exe", "not executable");
    Require(!validator.ValidateDirectory(root).Ok(), "undeclared EXE is rejected");
    std::filesystem::remove(root / L"undeclared.exe");
    Write(root / L"plugin.dll", "not a DLL");
    Require(!validator.ValidateDirectory(root).Ok(), "DLL extension remains forbidden");
    std::filesystem::remove(root / L"plugin.dll");
    auto missingTranslation = manifestText();
    const auto key = missingTranslation.find("\"purpose\":\"Show charge\"");
    missingTranslation.replace(key, std::string("\"purpose\":\"Show charge\"").size(), "\"other\":\"Show charge\"");
    Write(root / L"widget.json", missingTranslation);
    Require(!validator.ValidateDirectory(root).Ok(), "untranslated native consent key is rejected");
    Write(root / L"widget.json", manifestText());

    NativeRequestOptions parsed;
    Require(!ParseNativeRequestOptions({{"helper","../helper"},{"operation","read"}}, parsed), "helper ID cannot be a path");
    Require(!ParseNativeRequestOptions({{"helper","battery"},{"operation","read"},{"command","cmd.exe"}}, parsed), "arbitrary command argument is rejected");
    Require(ParseNativeRequestOptions({{"helper","battery"},{"operation","read"},{"payload","{\n\"x\":\"a b\"\n}"}}, parsed) && parsed.payload == "{\"x\":\"a b\"}", "pretty JSON compacts without changing string content");
    {
        Runner runner;
        Require(runner.executor.Start(1, root, fingerprint, {"battery","read", "{\n\"x\":1\n}", 3000}), "real helper starts");
        const auto messages = runner.Wait();
        Require(messages.size() == 2 && !messages.front().final && messages.back().final && messages.back().ok,
            "real pipe handshake, request, event, result and shutdown succeed");
        JsonValue frame;
        Require(ParseJson(messages.back().json, frame) && frame.Find("value")->Find("percent")->number == 73,
            "result comes from the actual helper protocol");
    }
    for (const auto& name : {"bad", "large", "failure", "hang"})
    {
        Runner runner;
        Require(runner.executor.Start(2, root, fingerprint, {"battery",name,"{}",1000}), "negative fixture starts");
        const auto messages = runner.Wait();
        const auto& final = messages.back();
        Require(final.final && !final.ok, "malformed, excessive, failed and timed-out helpers cannot succeed");
        if (std::string(name) == "hang") Require(final.error == "nativeTimeout", "non-responsive helper times out");
    }
    for (const auto& operation : {"bad-json", "gbk"})
    {
        Runner runner;
        Require(runner.executor.Start(21, root, fingerprint, {"battery",operation,"{}",3000}), "protocol diagnostic fixture starts");
        const auto messages = runner.Wait();
        const auto& final = messages.back();
        JsonValue diagnostic;
        Require(final.error == "nativeProtocolError" && ParseJson(final.diagnostic, diagnostic) &&
            diagnostic.Find("reason") && diagnostic.Find("phase") &&
            diagnostic.Find("phase")->string == "response" &&
            diagnostic.Find("reason")->string == (std::string(operation) == "gbk" ? "invalidUtf8" : "invalidJson") &&
            final.diagnostic.find("private token") == std::string::npos,
            "malformed JSON and invalid UTF-8 have distinct metadata without copying private stdout content");
    }
    {
        Runner runner;
        Require(runner.executor.Start(3, root, fingerprint, {"battery","hang","{}",5000}), "cancel fixture starts");
        const auto initial = runner.Wait(false);
        JsonValue frame;
        Require(!initial.front().final && ParseJson(initial.front().json, frame), "helper reports a live process");
        const DWORD pid = static_cast<DWORD>(frame.Find("value")->Find("pid")->number);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        Require(process != nullptr, "test observes helper process");
        Require(runner.executor.Cancel(3), "cancel reaches the real running operation");
        const auto messages = runner.Wait();
        Require(messages.back().error == "canceled", "canceled task cannot report success");
        Require(WaitForSingleObject(process, 1000) == WAIT_OBJECT_0, "cancel terminates the managed helper");
        CloseHandle(process);
    }
    {
        Runner runner;
        Require(runner.executor.Start(10, root, fingerprint, {"battery","watch","{}",1000,true}), "resident helper starts");
        HANDLE process = nullptr;
        DWORD pid = 0;
        std::size_t count = 0;
        while (count < 20)
        {
            for (const auto& message : runner.Wait(false))
            {
                Require(!message.final, "stream remains active beyond one-shot event and lifetime limits");
                JsonValue frame;
                Require(ParseJson(message.json, frame), "resident event is JSON");
                const auto currentPid = static_cast<DWORD>(frame.Find("value")->Find("pid")->number);
                if (!pid) { pid = currentPid; process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); }
                Require(currentPid == pid, "all resident readings come from the same process");
                ++count;
            }
        }
        Require(process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT, "resident helper stays alive while subscribed");
        Require(runner.executor.Cancel(10), "resident cancellation is accepted");
        Require(runner.Wait().back().error == "canceled", "resident cancellation completes once");
        DWORD exitCode = 1;
        Require(WaitForSingleObject(process, 1000) == WAIT_OBJECT_0 && GetExitCodeProcess(process, &exitCode) && exitCode == 0,
            "resident helper receives cooperative stop and exits normally");
        CloseHandle(process);
    }
    {
        HANDLE process = nullptr;
        {
            Runner runner;
            Require(runner.executor.Start(11, root, fingerprint, {"battery","watch","{}",1000,true}), "shutdown fixture starts");
            const auto messages = runner.Wait(false);
            JsonValue frame;
            Require(!messages.front().final && ParseJson(messages.front().json, frame), "shutdown fixture publishes a live PID");
            process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(frame.Find("value")->Find("pid")->number));
            Require(process != nullptr, "shutdown fixture process is observed");
        }
        Require(WaitForSingleObject(process, 1000) == WAIT_OBJECT_0, "executor shutdown leaves no resident process behind");
        CloseHandle(process);
    }
    {
        Runner runner;
        Require(runner.executor.Start(12, root, fingerprint, {"battery","watch","{}",1000,true}), "package-change fixture starts");
        Require(!runner.Wait(false).front().final, "package-change fixture starts streaming");
        Write(root / L"main.lua", "return widget.define({model={watchChanged=true}})");
        const auto messages = runner.Wait();
        JsonValue diagnostic;
        Require(messages.back().error == "nativeContentChanged" && ParseJson(messages.back().diagnostic, diagnostic) &&
            diagnostic.Find("reason") && diagnostic.Find("reason")->string == "packageChangeDetected" &&
            diagnostic.Find("relativePath") && diagnostic.Find("relativePath")->string == "main.lua",
            "a package write retires the resident helper and identifies its relative path");
        Write(root / L"main.lua", "return widget.define({model={}})");
    }
    for (const auto& operation : {"watch-silent", "watch-crash", "watch-burst"})
    {
        Runner runner;
        Require(runner.executor.Start(13, root, fingerprint, {"battery",operation,"{}",1000,true}), "resident failure fixture starts");
        const auto messages = runner.Wait();
        const auto expected = std::string(operation) == "watch-silent" ? "nativeTimeout" :
            (std::string(operation) == "watch-crash" ? "nativeExited" : "nativeOutputLimit");
        Require(messages.back().final && !messages.back().ok && messages.back().error == expected,
            "resident startup timeout, crash and output flooding terminate predictably");
    }
    Write(root / L"main.lua", "return widget.define({model={changed=true}})");
    Require(validator.ValidateDirectory(root, &manifest).Ok() && PackagePermissionScopeFingerprint(manifest) != fingerprint,
        "same-version script changes invalidate native authorization identity");
    {
        Runner runner;
        Require(runner.executor.Start(4, root, fingerprint, {"battery","read","{}",3000}), "stale content validation queues");
        const auto messages = runner.Wait();
        Require(messages.size() == 1 && messages.back().error == "nativeContentChanged", "changed package cannot launch under old approval");
    }

    PackagePaths paths;
    paths.builtin = temporary.path / L"builtin";
    paths.development = temporary.path / L"dev";
    paths.installed = temporary.path / L"installed";
    paths.staging = temporary.path / L"staging";
    paths.quarantine = temporary.path / L"quarantine";
    paths.migrations = temporary.path / L"migrations";
    paths.registry = temporary.path / L"packages.json";
    std::filesystem::create_directories(paths.development);
    std::filesystem::copy(root, paths.development / L"native", std::filesystem::copy_options::recursive);
    WidgetPackageManager manager(paths);
    std::string error;
    Require(manager.Initialize(error), "native package manager initializes");
    auto package = manager.Resolve(manifest.id);
    Require(package && package->permissionState == PermissionDecisionState::Pending, "native development package requires explicit consent");
    Require(!manager.SetPermissionDecision(manifest.id, PermissionDecisionState::Granted, {"native.execute"}, {}, error) &&
        error == "nativeConfirmationRequired", "legacy permission flows cannot grant native execution");
    const auto review = PackagePermissionReviewFingerprint(*package);
    auto otherSource = *package;
    otherSource.source.providerId = "other-source";
    Require(!manager.SetPermissionDecision(manifest.id, PermissionDecisionState::Granted, {"native.execute"}, {}, error, true,
        PackagePermissionReviewFingerprint(otherSource)) && error == "nativeContentChanged", "review is bound to the actual package source");
    Require(manager.SetPermissionDecision(manifest.id, PermissionDecisionState::Granted, {"native.execute"}, {}, error, true, review), "fixture records explicit strong approval");
    Require(manager.Resolve(manifest.id)->permissionState == PermissionDecisionState::Granted, "unchanged content retains approval");
    Write(paths.development / L"native" / L"main.lua", "return widget.define({model={again=true}})");
    Require(!manager.SetPermissionDecision(manifest.id, PermissionDecisionState::Granted, {"native.execute"}, {}, error, true, review) &&
        error == "nativeContentChanged", "content changes during the consent dialog cannot acquire the old approval");
    bool scopeChanged = false;
    Require(manager.RefreshChangedPermissionScope(manifest.id, scopeChanged, error) && scopeChanged &&
        manager.Resolve(manifest.id)->permissionState == PermissionDecisionState::Pending,
        "same-version package update returns to pending consent");
    Write(root / L"helper.exe", "changed executable");
    Require(!validator.ValidateDirectory(root).Ok(), "tampered EXE hash is rejected");
    {
        Runner runner;
        Require(runner.executor.Start(22, root, fingerprint, {"battery","read","{}",3000}), "changed image validation queues");
        const auto messages = runner.Wait();
        Require(messages.back().error == "nativeContentChanged" &&
            messages.back().diagnostic.find("helperHashMismatch") != std::string::npos,
            "a stale helper hash is diagnosed before launch without relaxing authorization");
    }
}
}
int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--snowdesktop-native-v1") return Helper();
    try
    {
        Tests();
        wchar_t managed[32768]{};
        const DWORD count = GetEnvironmentVariableW(L"SNOWDESKTOP_NATIVE_TEST_HELPER", managed, 32768);
        if (count)
        {
            Require(count < 32768, "managed test helper path fits");
            ProbeManagedHelper(managed);
            std::cout << ".NET Framework fixed-argument / UTF-8 / CP936 probes passed\n";
        }
        wchar_t demo[32768]{};
        const DWORD demoCount = GetEnvironmentVariableW(L"SNOWDESKTOP_NATIVE_DEMO_PACKAGE", demo, 32768);
        if (demoCount)
        {
            Require(demoCount < 32768, "actual demo package path fits");
            ProbeDemoPackage(demo);
        }
        std::cout << "native helper integration tests passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
