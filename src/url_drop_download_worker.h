#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace snowdesktop
{

struct UrlDropDownloadRequest
{
    std::wstring url;
    std::filesystem::path destinationDirectory;
    std::uint64_t maximumBytes = 64ull * 1024ull * 1024ull;
    int timeoutMs = 10000;
};

enum class UrlDropDownloadOutcome
{
    Downloaded,
    Shortcut,
    Failed,
};

struct UrlDropDownloadResult
{
    UrlDropDownloadOutcome outcome = UrlDropDownloadOutcome::Failed;
    std::wstring originalUrl;
    std::wstring finalUrl;
    std::wstring localPath;
    std::wstring contentType;
    std::string error;
};

/**
 * Serial background resolver for URL-only desktop drops.
 *
 * Network and attachment-policy work stays off the desktop OLE thread. The
 * HTTP(S) resources may be downloaded; unsupported or rejected responses are
 * reported as shortcuts. The completion runs on the worker and must marshal
 * any UI work itself.
 */
class UrlDropDownloadWorker
{
public:
    using Completion = std::function<void(UrlDropDownloadResult)>;

    UrlDropDownloadWorker() = default;
    ~UrlDropDownloadWorker();

    UrlDropDownloadWorker(const UrlDropDownloadWorker&) = delete;
    UrlDropDownloadWorker& operator=(const UrlDropDownloadWorker&) = delete;

    bool Enqueue(UrlDropDownloadRequest request, Completion completion);
    void Stop();

    static UrlDropDownloadResult Execute(
        const UrlDropDownloadRequest& request, std::stop_token token);

private:
    struct Task
    {
        UrlDropDownloadRequest request;
        Completion completion;
    };

    void Run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::stop_source activeStopSource_;
    bool started_ = false;
    bool executing_ = false;
    bool stopping_ = false;
};

} // namespace snowdesktop
