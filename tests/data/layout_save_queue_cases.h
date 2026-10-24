#pragma once

#include "data/layout_save_queue.h"
#include "data/layout_storage.h"
#include <future>
#include <fstream>
#include <stdexcept>

namespace layout_save_queue_cases
{
inline void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
inline std::string Read(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
inline snowdesktop::LayoutSaveQueue::Write Writer(const std::filesystem::path& path, int column)
{
    const auto document = "{\"pages\":[{\"id\":\"page\",\"columns\":8,\"rows\":8}],"
        "\"items\":[{\"key\":\"file\",\"page\":\"page\",\"column\":" +
        std::to_string(column) + ",\"row\":0}]}";
    return [path, document] {
        snowdesktop::LayoutSaveQueue::Result result;
        result.saved = snowdesktop::layout_storage::SaveDocument(path, document, &result.error);
        return result;
    };
}
inline void Run(const std::filesystem::path& root)
{
    using Queue = snowdesktop::LayoutSaveQueue;
    using namespace std::chrono_literals;
    const auto path = root / L"async-layout" / L"layout.json";
    std::filesystem::create_directories(path.parent_path());
    Require(Writer(path, 0)().saved, "initialize real layout before asynchronous writes");
    Queue queue;
    std::promise<void> started, release;
    auto released = release.get_future().share();
    auto first = Writer(path, 1);
    Require(queue.Submit([&started, released, first] {
            started.set_value();
            if (released.wait_for(5s) != std::future_status::ready)
                return Queue::Result{false, "test writer release timed out"};
            return first();
        }, [](auto) {}, nullptr, 0), "start blocked layout writer");
    Require(started.get_future().wait_for(2s) == std::future_status::ready,
        "writer starts outside the submitting thread");
    std::atomic<unsigned> intermediate{0};
    Require(queue.Submit([&intermediate, write = Writer(path, 2)] {
            ++intermediate; return write();
        }, [](auto) {}, nullptr, 0), "queue intermediate snapshot while prior writer is blocked");
    Require(queue.Submit(Writer(path, 3), [](auto) {}, nullptr, 0),
        "replace pending snapshot without waiting for the disk writer");
    release.set_value();
    const auto result = queue.Flush();
    Require(result && result->saved && intermediate == 0,
        "flush writes the latest snapshot and coalesces the superseded pending layout");
    Require(Read(path).find("\"column\":3") != std::string::npos,
        "real persisted layout contains the latest accepted position");
    std::string error;
    snowdesktop::layout_storage::Document document;
    Require(snowdesktop::layout_storage::ParseDocument(Read(path), document, &error),
        "asynchronous output remains a valid typed layout document");

    Require(queue.Submit(Writer(path, 4), [](auto) {}, nullptr, 0), "queue older snapshot before synchronous save");
    Require(queue.SaveNow(Writer(path, 5)).saved, "synchronous boundary supersedes and waits for earlier work");
    queue.Drain();
    Require(Read(path).find("\"column\":5") != std::string::npos,
        "an older queued or running save cannot overwrite the synchronous final layout");
    const auto good = Read(path);
    Require(queue.Submit([path] {
            Queue::Result failed;
            failed.saved = snowdesktop::layout_storage::SaveDocument(path, "{invalid", &failed.error);
            return failed;
        }, [](auto) {}, nullptr, 0), "queue invalid document through the real production validator");
    const auto failed = queue.Flush();
    Require(failed && !failed->saved && !failed->error.empty() && Read(path) == good,
        "background validation failure is observable and preserves the last persisted layout");
    {
        Queue exiting;
        Require(exiting.Submit(Writer(path, 6), [](auto) {}, nullptr, 0), "queue final document before writer teardown");
    }
    Require(Read(path).find("\"column\":6") != std::string::npos,
        "writer teardown flushes the accepted final snapshot before releasing ownership");
}
}
