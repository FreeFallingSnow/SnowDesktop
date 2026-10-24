#include "widget/tasks/widget_filesystem_watch_service.h"
#include "widget/tasks/widget_filesystem_watch_origin.h"
#include "widget/widget_script_read_ahead_cases.h"

#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void RemoveProbeDirectory(const std::filesystem::path& root)
{
    wchar_t temporary[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporary) > 0, "cleanup temporary directory is available");
    const auto absolute = std::filesystem::absolute(root).lexically_normal();
    Expect(std::filesystem::equivalent(absolute.parent_path(), std::filesystem::weakly_canonical(temporary)) &&
            absolute.filename() == L"SnowDesktopWidgetFilesystemWatchServiceTests-" +
                std::to_wstring(GetCurrentProcessId()),
        "recursive watch cleanup stays in the exact owned temporary directory");
    Expect((GetFileAttributesW(absolute.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ||
            !std::filesystem::exists(absolute), "the cleanup root is not a substituted reparse point");
    std::error_code error;
    std::filesystem::remove_all(absolute, error);
    Expect(!error, "owned watch fixture cleanup succeeds");
}

std::filesystem::path CreateProbeDirectory()
{
    wchar_t temporary[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporary) > 0,
        "temporary directory is available");
    const auto root = std::filesystem::path(temporary) /
        (L"SnowDesktopWidgetFilesystemWatchServiceTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    RemoveProbeDirectory(root);
    error.clear();
    Expect(std::filesystem::create_directories(root, error) && !error,
        "probe directory is created");
    return std::filesystem::weakly_canonical(root);
}

std::vector<snowdesktop::widget_runtime::WidgetFilesystemWatchCompletion>
WaitFor(snowdesktop::widget_runtime::WidgetFilesystemWatchService& service,
    const std::function<bool(const snowdesktop::widget_runtime::
        WidgetFilesystemWatchCompletion&)>& predicate)
{
    std::vector<snowdesktop::widget_runtime::
        WidgetFilesystemWatchCompletion> observed;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto completions = service.DrainCompletions();
        for (auto& completion : completions)
        {
            const bool matched = predicate(completion);
            observed.push_back(std::move(completion));
            if (matched) return observed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("filesystem watch completion timed out");
}

bool HasEvent(const snowdesktop::widget_runtime::
    WidgetFilesystemWatchCompletion& completion,
    std::string_view kind, std::wstring_view name)
{
    for (const auto& event : completion.events)
    {
        if (event.kind == kind && event.name == name) return true;
    }
    return false;
}

void CreateFixtureJunction(const std::filesystem::path& fixture,
    const std::filesystem::path& link, const std::filesystem::path& target)
{
    for (const auto& path : {link, target})
    {
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        auto rootPart = fixture.begin();
        auto pathPart = absolute.begin();
        for (; rootPart != fixture.end() && pathPart != absolute.end(); ++rootPart, ++pathPart)
            Expect(*rootPart == *pathPart, "junction target stays in the owned fixture");
        Expect(rootPart == fixture.end() && pathPart != absolute.end(), "junction is strictly inside the fixture");
    }
    std::filesystem::create_directory(link);
    const std::wstring substitute = L"\\??\\" + target.wstring();
    const std::wstring display = target.wstring();
    struct MountPoint
    {
        DWORD tag;
        WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength;
        wchar_t names[1];
    };
    const auto nameBytes = (substitute.size() + display.size() + 2) * sizeof(wchar_t);
    std::vector<unsigned char> bytes(offsetof(MountPoint, names) + nameBytes);
    auto* point = reinterpret_cast<MountPoint*>(bytes.data());
    point->tag = IO_REPARSE_TAG_MOUNT_POINT;
    point->length = static_cast<WORD>(8 + nameBytes);
    point->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    point->printOffset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    point->printLength = static_cast<WORD>(display.size() * sizeof(wchar_t));
    std::memcpy(point->names, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(point->names) + point->printOffset,
        display.c_str(), (display.size() + 1) * sizeof(wchar_t));
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Expect(handle != INVALID_HANDLE_VALUE, "fixture junction can be opened without following it");
    DWORD returned = 0;
    const bool created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT,
        point, static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    Expect(created, "the isolated native junction is created");
}

void TestOriginPreservesGrantBoundary(const std::filesystem::path& root)
{
    using namespace snowdesktop::widget_runtime;
    const auto granted = root / L"granted";
    const auto directory = granted / L"child";
    const auto outside = root / L"outside";
    std::filesystem::create_directories(directory);
    std::filesystem::create_directories(outside / L"child");
    WidgetFilesystemHandleStore store(root / L"handles.json");
    std::string error;
    Expect(store.Load(error), "real isolated handle store loads");
    const WidgetFilesystemHandleOwner owner{"origin-owner", "origin-package"};
    WidgetFilesystemWatchService service;
    Expect(static_cast<bool>(service.Start(100, owner.instanceId, directory)), "normal authorized watch is queued");
    (void)WaitFor(service, [](const auto& value) { return value.id == 100 && value.kind == WidgetFilesystemWatchCompletionKind::Started; });
    { std::ofstream file(directory / L"marker.txt"); file << "authorized"; }
    auto oldEvents = WaitFor(service, [](const auto& value) { return value.id == 100 && HasEvent(value, "added", L"marker.txt"); });
    std::shared_ptr<const WidgetFilesystemWatchOrigin> oldOrigin;
    for (const auto& completion : oldEvents)
        if (HasEvent(completion, "added", L"marker.txt")) oldOrigin = completion.origin;
    Expect(static_cast<bool>(oldOrigin), "real event completion retains its opened directory identity");
    const auto first = oldOrigin->GrantChild(store, owner, L"marker.txt", WidgetFilesystemHandleAccess::Read);
    Expect(first && store.Resolve(owner, first.entry->handle)->path == directory / L"marker.txt",
        "normal actual watch event derives the expected child capability");
    Expect(service.Stop(100), "old origin watch stops");
    (void)WaitFor(service, [](const auto& value) { return value.id == 100 && value.kind == WidgetFilesystemWatchCompletionKind::Stopped; });

    const auto moved = root / L"moved-granted";
    std::filesystem::rename(granted, moved);
    { std::ofstream file(outside / L"child" / L"marker.txt"); file << "outside authorization"; }
    struct JunctionCleanup
    {
        std::filesystem::path path;
        ~JunctionCleanup()
        {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                RemoveDirectoryW(path.c_str());
        }
    } junctionCleanup{granted};
    CreateFixtureJunction(root, granted, outside);
    Expect(!oldOrigin->GrantChild(store, owner, L"marker.txt", WidgetFilesystemHandleAccess::ReadWrite),
        "a retained event after Stop cannot grant through an ancestor replaced by a junction");
    Expect(static_cast<bool>(service.Start(101, owner.instanceId, directory)), "redirected watch request reaches the real worker");
    const auto redirected = WaitFor(service, [](const auto& value) {
        return value.id == 101 && (value.kind == WidgetFilesystemWatchCompletionKind::Started ||
            value.kind == WidgetFilesystemWatchCompletionKind::Error);
    });
    Expect(redirected.back().kind == WidgetFilesystemWatchCompletionKind::Error,
        "starting or restoring a watch rejects an intermediate junction before outside events can be observed");
    (void)service.Stop(101);
    Expect(RemoveDirectoryW(granted.c_str()) != FALSE, "unlink only the owned junction, preserving its separate target");
    std::filesystem::create_directories(directory);
    { std::ofstream file(directory / L"marker.txt"); file << "different ordinary directory"; }
    Expect(!oldOrigin->GrantChild(store, owner, L"marker.txt", WidgetFilesystemHandleAccess::Read),
        "ordinary directory replacement also cannot reuse an old origin identity");
    Expect(static_cast<bool>(service.Start(100, owner.instanceId, directory)), "the same subscription can restart with a fresh ordinary origin");
    (void)WaitFor(service, [](const auto& value) { return value.id == 100 && value.kind == WidgetFilesystemWatchCompletionKind::Started; });
    { std::ofstream file(directory / L"fresh.txt"); file << "new origin"; }
    const auto fresh = WaitFor(service, [](const auto& value) { return value.id == 100 && HasEvent(value, "added", L"fresh.txt"); });
    for (const auto& completion : fresh)
        if (HasEvent(completion, "added", L"fresh.txt"))
            Expect(completion.origin && completion.origin != oldOrigin &&
                    completion.origin->GrantChild(store, owner, L"fresh.txt", WidgetFilesystemHandleAccess::Read),
                "new events retain a different origin and still grant their actual authorized child");
    Expect(service.Stop(100), "new origin stops cleanly");
    (void)WaitFor(service, [](const auto& value) { return value.id == 100 && value.kind == WidgetFilesystemWatchCompletionKind::Stopped; });
    Expect(store.Size() == 2, "only the two legitimate child capabilities were persisted");
}
}

int RunWatchTests()
{
    using namespace snowdesktop::widget_runtime;
    const auto root = CreateProbeDirectory();
    struct ProbeCleanup
    {
        std::filesystem::path root;
        ~ProbeCleanup() noexcept { try { if (!root.empty()) RemoveProbeDirectory(root); } catch (...) {} }
    } cleanup{root};
    WidgetFilesystemWatchService service;

    Expect(static_cast<bool>(service.Start(1, "instance", root)),
        "directory watch request is accepted");
    (void)WaitFor(service, [](const auto& completion) {
        return completion.id == 1 && completion.kind ==
            WidgetFilesystemWatchCompletionKind::Started;
    });
    Expect(service.RequestedCount() == 1,
        "active request is counted");

    const auto first = root / L"first.txt";
    std::ofstream(first, std::ios::binary) << "one";
    (void)WaitFor(service, [](const auto& completion) {
        return completion.id == 1 && completion.kind ==
                WidgetFilesystemWatchCompletionKind::Events &&
            (HasEvent(completion, "added", L"first.txt") ||
                HasEvent(completion, "modified", L"first.txt"));
    });

    const auto renamed = root / L"renamed.txt";
    std::filesystem::rename(first, renamed);
    (void)WaitFor(service, [](const auto& completion) {
        if (completion.id != 1 || completion.kind !=
                WidgetFilesystemWatchCompletionKind::Events)
            return false;
        for (const auto& event : completion.events)
        {
            if (event.kind == "renamed" &&
                event.oldName == L"first.txt" &&
                event.name == L"renamed.txt")
                return true;
        }
        return false;
    });

    Expect(service.Stop(1), "active watch can be stopped");
    (void)WaitFor(service, [](const auto& completion) {
        return completion.id == 1 && completion.kind ==
            WidgetFilesystemWatchCompletionKind::Stopped;
    });
    Expect(service.RequestedCount() == 0,
        "stopped request is removed");

    Expect(static_cast<bool>(service.Start(2, "instance", root)),
        "second watch request is accepted");
    (void)WaitFor(service, [](const auto& completion) {
        return completion.id == 2 && completion.kind ==
            WidgetFilesystemWatchCompletionKind::Started;
    });
    Expect(service.ForgetInstance("instance") == 1,
        "instance disposal removes its watches");
    (void)WaitFor(service, [](const auto& completion) {
        return completion.id == 2 && completion.kind ==
            WidgetFilesystemWatchCompletionKind::Stopped;
    });

    TestOriginPreservesGrantBoundary(root);
    RemoveProbeDirectory(root);
    cleanup.root.clear();
    std::cout << "widget filesystem watch service tests passed\n";
    return 0;
}

int main()
{
    try
    {
        widget_script_read_ahead_tests::Run();
        return RunWatchTests();
    }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
