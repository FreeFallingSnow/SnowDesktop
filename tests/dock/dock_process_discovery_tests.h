#pragma once

#include "dock/dock_process_discovery.h"
#include "dock/dock_app_identity_rules.h"
#include <memory>
#include <optional>
#include <string_view>

namespace snowdesktop::tests::dock_process_discovery
{
namespace discovery = snowdesktop::dock_process_discovery;

struct FakeImage
{
    struct State
    {
        std::wstring path;
        bool alive = true;
        bool cacheable = true;
        bool readable = true;
        unsigned reads = 0;
    };
    static inline std::unordered_map<DWORD, std::shared_ptr<State>> current;
    std::shared_ptr<State> state;
    static FakeImage Open(DWORD processId)
    {
        const auto found = current.find(processId);
        return {found == current.end() ? nullptr : found->second};
    }
    bool CanReuse() const { return state && state->alive && state->cacheable; }
    std::wstring ReadPath() const
    {
        if (!state) return {};
        ++state->reads;
        return state->readable ? state->path : std::wstring{};
    }
};

inline std::optional<int> TryRunHelper(int argc, char** argv)
{
    if (argc != 3 || std::string_view(argv[1]) != "--dock-process-fixture") return {};
    const std::string name(argv[2]);
    const std::wstring wideName(name.begin(), name.end());
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, wideName.c_str());
    if (!stop) return 2;
    const DWORD result = WaitForSingleObject(stop, 30000);
    CloseHandle(stop);
    return result == WAIT_OBJECT_0 ? 0 : 3;
}

template<class Check>
void Run(Check check, bool oldRepeatedQueries = false)
{
    // Only the OS process boundary is replaced here: the production cache and
    // bounded ancestor walker still execute. The old-query control bypasses
    // sharing exactly where separate child-process chains used to query again.
    FakeImage::current.clear();
    const auto normalize = [](std::wstring path) { return path; };
    auto launcher = std::make_shared<FakeImage::State>();
    launcher->path = L"C:\\APP\\LAUNCHER.EXE";
    auto root = std::make_shared<FakeImage::State>();
    root->path = L"C:\\SYSTEM\\ROOT.EXE";
    FakeImage::current = {{10, launcher}, {5, root}};
    const discovery::ParentMap parents{{100, 10}, {101, 10}, {10, 5}};
    {
        discovery::ExecutablePaths<FakeImage> cache;
        const auto read = [&](DWORD pid) {
            return oldRepeatedQueries ? FakeImage::Open(pid).ReadPath() : cache.Read(pid, normalize);
        };
        const auto first = discovery::AncestorPaths(100, parents, read);
        const auto second = discovery::AncestorPaths(101, parents, read);
        const std::vector<std::wstring> expected{L"C:\\APP\\LAUNCHER.EXE", L"C:\\SYSTEM\\ROOT.EXE"};
        check(first == expected && second == expected, "sibling processes retain nearest-first launcher ancestry");
        check(snowdesktop::dock_app_identity_rules::MatchesRunningApp(
            DockAppIdentityKind::Executable, launcher->path, {}, {}, L"C:\\APP\\RENDER.EXE", {}, second),
            "shared ancestry preserves the pinned launcher match for a child executable");
        check(launcher->reads == 1 && root->reads == 1,
            "sibling chains query each live ancestor image only once per enumeration");
    }
    check(launcher.use_count() == 2 && root.use_count() == 2,
        "enumeration destruction releases retained process identities");
    {
        discovery::ExecutablePaths<FakeImage> nextPass;
        nextPass.Read(10, normalize);
        check(launcher->reads == (oldRepeatedQueries ? 3u : 2u),
            "a new enumeration queries the current process again");
    }
    {
        discovery::ExecutablePaths<FakeImage> cache;
        check(cache.Read(77, normalize).empty(), "an unavailable process remains an empty query result");
        FakeImage::current[77] = launcher;
        check(cache.Read(77, normalize) == launcher->path,
            "a process missing on its first query is discovered in the same pass");
        check(cache.Read(10, normalize) == launcher->path, "initial process creation is readable");
        launcher->alive = false;
        auto replacement = std::make_shared<FakeImage::State>();
        replacement->path = L"C:\\OTHER\\REUSED-PID.EXE";
        FakeImage::current[10] = replacement;
        check(cache.Read(10, normalize) == replacement->path,
            "an exited process cannot supply the path for a reused PID");
        replacement->readable = false;
        replacement->alive = false;
        check(cache.Read(10, normalize).empty(), "exit invalidates a previously successful path");
        replacement->alive = true;
        replacement->readable = true;
        check(cache.Read(10, normalize) == replacement->path,
            "a failed query can recover during the same enumeration");
        replacement->cacheable = false;
        const unsigned before = replacement->reads;
        cache.Read(10, normalize);
        cache.Read(10, normalize);
        check(replacement->reads == before + 2,
            "unverifiable process lifetimes preserve uncached image queries");
    }
    unsigned calls = 0;
    const auto readNumber = [&](DWORD pid) { ++calls; return std::to_wstring(pid); };
    check(discovery::AncestorPaths(1, {{1, 2}, {2, 3}, {3, 1}}, readNumber) ==
        std::vector<std::wstring>{L"2", L"3"}, "cyclic ancestry stops before reintroducing the child");
    check(discovery::AncestorPaths(1, {{1, 0}}, readNumber).empty() &&
        discovery::AncestorPaths(1, {}, readNumber).empty(), "missing and zero parents remain empty");
    discovery::ParentMap deep;
    for (DWORD pid = 1; pid <= 40; ++pid) deep[pid] = pid + 1;
    calls = 0;
    const auto bounded = discovery::AncestorPaths(1, deep, readNumber);
    check(bounded.size() == 32 && bounded.front() == L"2" && bounded.back() == L"33" && calls == 32,
        "ancestry retains the existing 32-level query bound");
    const auto partial = discovery::AncestorPaths(1, deep,
        [](DWORD pid) { return pid == 3 ? std::wstring{} : std::to_wstring(pid); });
    check(partial == std::vector<std::wstring>{L"2"}, "an unreadable ancestor stops the chain at the same point");
    FakeImage::current.clear();

    // Real Win32 process handles/path APIs, with no window and no desktop-host
    // interaction. Re-enter this binary only as an event-controlled fixture.
    const std::wstring eventName = L"Local\\SnowDesktopDockProcess-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    check(stop != nullptr, "process lifetime fixture creates a private stop event");
    if (!stop) return;
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --dock-process-fixture " + eventName;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(executable, command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    check(started != FALSE, "process lifetime fixture starts without a window");
    if (started)
    {
        CloseHandle(child.hThread);
        auto image = discovery::ProcessImage::Open(child.dwProcessId);
        check(image.CanReuse() && !image.ReadPath().empty(), "real live process image has a verifiable creation identity");
        discovery::ExecutablePaths<> cache;
        const auto path = cache.Read(child.dwProcessId, normalize);
        check(!path.empty() && path == image.ReadPath() && cache.Read(child.dwProcessId, normalize) == path,
            "cached Win32 image equals the direct process query");
        SetEvent(stop);
        check(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0,
            "the private process exits within the fixture deadline");
        check(!image.CanReuse(), "a signaled real process identity cannot be reused");
        CloseHandle(child.hProcess);
    }
    CloseHandle(stop);
}
}
