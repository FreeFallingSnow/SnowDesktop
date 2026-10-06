#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetFilesystemWatchEvent
{
    std::string kind;
    std::wstring name;
    std::wstring oldName;
};

enum class WidgetFilesystemWatchCompletionKind
{
    Started,
    Events,
    Stopped,
    Error,
};

struct WidgetFilesystemWatchCompletion
{
    std::uint64_t id = 0;
    WidgetFilesystemWatchCompletionKind kind =
        WidgetFilesystemWatchCompletionKind::Error;
    std::vector<WidgetFilesystemWatchEvent> events;
    bool overflow = false;
    std::string error;
};

struct WidgetFilesystemWatchStartResult
{
    bool started = false;
    std::string error;

    explicit operator bool() const noexcept
    {
        return started && error.empty();
    }
};

class WidgetFilesystemWatchService
{
public:
    static constexpr std::size_t MaximumPendingEvents = 256;

    WidgetFilesystemWatchService();
    ~WidgetFilesystemWatchService();

    WidgetFilesystemWatchService(
        const WidgetFilesystemWatchService&) = delete;
    WidgetFilesystemWatchService& operator=(
        const WidgetFilesystemWatchService&) = delete;

    WidgetFilesystemWatchStartResult Start(std::uint64_t id,
        std::string instanceId, std::filesystem::path directory);
    bool Stop(std::uint64_t id);
    std::size_t ForgetInstance(std::string_view instanceId);
    std::vector<WidgetFilesystemWatchCompletion> DrainCompletions();
    std::size_t RequestedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
