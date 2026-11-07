#pragma once

#include "widget_filesystem_handle_store.h"
#include "widget/runtime/widget_runtime_image.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
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
    bool grantHandles = true;
    std::string name;
    std::size_t maxDimension = 2048;
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
    std::shared_ptr<const WidgetRuntimeImagePixels> image;
    std::string resourceToken;
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
    bool grantHandles = true;
    std::shared_ptr<const WidgetRuntimeImagePixels> image;
    std::string resourceToken;
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
    // Legacy injected runners retain their one-argument contract. Production
    // actions and cooperative test runners observe a per-request stop token.
    using CooperativeRunner = std::function<WidgetFilesystemTaskRunResult(
        const WidgetFilesystemTaskRequest& request, std::stop_token stopToken)>;
    using NowProvider = std::function<Clock::time_point()>;
    using CompletionCallback = std::function<void()>;

    static constexpr std::size_t MaximumTextBytes = 1024 * 1024;
    static constexpr std::size_t MaximumListLimit = 100;
    static constexpr std::size_t MaximumListOffset = 10000;
    static constexpr std::size_t MaximumDirectoryEntries = 10000;
    static constexpr auto MinimumWriteInterval =
        std::chrono::milliseconds(100);

    explicit WidgetFilesystemTaskExecutor(
        Runner runner = {}, NowProvider nowProvider = {},
        CooperativeRunner cooperativeRunner = {});
    ~WidgetFilesystemTaskExecutor();

    WidgetFilesystemTaskExecutor(
        const WidgetFilesystemTaskExecutor&) = delete;
    WidgetFilesystemTaskExecutor& operator=(
        const WidgetFilesystemTaskExecutor&) = delete;

    WidgetFilesystemTaskStartResult Start(std::uint64_t id,
        std::string instanceId, WidgetFilesystemTaskRequest request);
    bool Cancel(std::uint64_t id);
    // Notification only: the host posts to its UI thread before draining.
    void SetCompletionCallback(CompletionCallback callback);
    void ForgetInstance(std::string_view instanceId);
    std::vector<WidgetFilesystemTaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

    static bool SupportsAction(std::string_view action) noexcept;
    static bool ValidateRequest(
        const WidgetFilesystemTaskRequest& request) noexcept;
    static bool IsDirectChildName(std::string_view name) noexcept;

private:
    struct QueuedRequest
    {
        std::uint64_t id = 0;
        std::string instanceId;
        WidgetFilesystemTaskRequest request;
        std::shared_ptr<std::stop_source> stop;
    };

    static WidgetFilesystemTaskRunResult RunSystemAction(
        const WidgetFilesystemTaskRequest& request, std::stop_token stopToken);
    // A WIC decode cannot be interrupted inside CopyPixels. An in-flight image
    // owns this state independently of the executor and cannot retain the host.
    struct State
    {
        CooperativeRunner runner;
        std::mutex mutex;
        std::mutex completionMutex;
        std::condition_variable condition;
        std::deque<QueuedRequest> requests;
        std::unordered_set<std::uint64_t> active;
        std::unordered_set<std::uint64_t> canceled;
        std::unordered_map<std::uint64_t, std::shared_ptr<std::stop_source>> requestStops;
        std::unordered_map<std::string, Clock::time_point> lastWrites;
        std::vector<WidgetFilesystemTaskCompletion> completions;
        CompletionCallback completionCallback;
        bool stopping = false;
        bool runningImage = false;
    };
    static void WorkerMain(const std::shared_ptr<State>& state,
        std::stop_token stopToken);

    std::shared_ptr<State> state_;
    NowProvider nowProvider_;
    std::jthread worker_;
};
}
