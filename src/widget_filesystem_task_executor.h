#pragma once

#include "widget_filesystem_handle_store.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetFilesystemMetadata
{
    WidgetFilesystemHandleKind kind = WidgetFilesystemHandleKind::File;
    std::filesystem::path path;
    std::string handle;
    std::string name;
    std::uint64_t size = 0;
    std::int64_t modifiedMs = 0;
    bool readOnly = false;
    std::string revision;
};

struct WidgetFilesystemTaskRequest
{
    std::string action;
    std::filesystem::path path;
    std::string handle;
    std::size_t offset = 0;
    std::size_t limit = 50;
    std::size_t maxBytes = 512 * 1024;
    std::string encoding = "utf8";
    std::string text;
    std::string expectedRevision;
};

struct WidgetFilesystemTaskRunResult
{
    bool ok = false;
    WidgetFilesystemMetadata metadata;
    std::vector<WidgetFilesystemMetadata> items;
    std::string text;
    std::size_t nextOffset = 0;
    bool hasMore = false;
    std::string error;
    std::string encoding = "utf8";
};

struct WidgetFilesystemTaskCompletion
{
    std::uint64_t id = 0;
    std::string action;
    bool ok = false;
    WidgetFilesystemMetadata metadata;
    std::vector<WidgetFilesystemMetadata> items;
    std::string text;
    std::size_t nextOffset = 0;
    bool hasMore = false;
    std::string error;
    std::string encoding = "utf8";
};

struct WidgetFilesystemTaskStartResult
{
    bool started = false;
    std::string error;

    explicit operator bool() const noexcept
    {
        return started && error.empty();
    }
};

class WidgetFilesystemTaskExecutor
{
public:
    using Clock = std::chrono::steady_clock;
    using Runner = std::function<WidgetFilesystemTaskRunResult(
        const WidgetFilesystemTaskRequest& request)>;
    using NowProvider = std::function<Clock::time_point()>;

    static constexpr std::size_t MaximumTextBytes = 1024 * 1024;
    static constexpr std::size_t MaximumListLimit = 100;
    static constexpr std::size_t MaximumListOffset = 10000;
    static constexpr std::size_t MaximumDirectoryEntries = 10000;
    static constexpr auto MinimumWriteInterval =
        std::chrono::milliseconds(100);

    explicit WidgetFilesystemTaskExecutor(
        Runner runner = {}, NowProvider nowProvider = {});
    ~WidgetFilesystemTaskExecutor();

    WidgetFilesystemTaskExecutor(
        const WidgetFilesystemTaskExecutor&) = delete;
    WidgetFilesystemTaskExecutor& operator=(
        const WidgetFilesystemTaskExecutor&) = delete;

    WidgetFilesystemTaskStartResult Start(std::uint64_t id,
        std::string instanceId, WidgetFilesystemTaskRequest request);
    bool Cancel(std::uint64_t id);
    void ForgetInstance(std::string_view instanceId);
    std::vector<WidgetFilesystemTaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

    static bool SupportsAction(std::string_view action) noexcept;
    static bool ValidateRequest(
        const WidgetFilesystemTaskRequest& request) noexcept;

private:
    struct QueuedRequest
    {
        std::uint64_t id = 0;
        std::string instanceId;
        WidgetFilesystemTaskRequest request;
    };

    static WidgetFilesystemTaskRunResult RunSystemAction(
        const WidgetFilesystemTaskRequest& request);
    void WorkerMain(std::stop_token stopToken);

    Runner runner_;
    NowProvider nowProvider_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueuedRequest> requests_;
    std::unordered_set<std::uint64_t> active_;
    std::unordered_set<std::uint64_t> canceled_;
    std::unordered_map<std::string, Clock::time_point> lastWrites_;
    std::vector<WidgetFilesystemTaskCompletion> completions_;
    std::jthread worker_;
    bool stopping_ = false;
};
}
