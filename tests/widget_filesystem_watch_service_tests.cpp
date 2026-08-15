#include "widget_filesystem_watch_service.h"

#include <windows.h>

#include <chrono>
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

std::filesystem::path CreateProbeDirectory()
{
    wchar_t temporary[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporary) > 0,
        "temporary directory is available");
    const auto root = std::filesystem::path(temporary) /
        (L"SnowDesktopWidgetFilesystemWatchServiceTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
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
}

int main()
{
    using namespace snowdesktop::widget_runtime;
    const auto root = CreateProbeDirectory();
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

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::cout << "widget filesystem watch service tests passed\n";
    return 0;
}
