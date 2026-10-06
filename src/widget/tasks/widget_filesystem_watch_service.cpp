#include "widget_filesystem_watch_service.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_CREATION;
constexpr std::size_t kNotificationBufferBytes = 64 * 1024;

bool IsSafeDirectName(std::wstring_view name)
{
    return !name.empty() && name != L"." && name != L".." &&
        name.find(L'\\') == std::wstring_view::npos &&
        name.find(L'/') == std::wstring_view::npos &&
        name.find(L'\0') == std::wstring_view::npos;
}
}

struct WidgetFilesystemWatchService::Impl
{
    enum class CommandKind { Start, Stop, Shutdown };

    struct Request
    {
        std::string instanceId;
        std::filesystem::path directory;
    };

    struct Command
    {
        CommandKind kind = CommandKind::Stop;
        std::uint64_t id = 0;
        Request request;
    };

    struct Watch
    {
        std::uint64_t id = 0;
        HANDLE directory = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        std::vector<unsigned char> buffer;
        std::wstring pendingRenameOld;
        bool pending = false;
        bool stopping = false;
    };

    HANDLE completionPort = nullptr;
    mutable std::mutex commandMutex;
    std::deque<Command> commands;
    std::unordered_map<std::uint64_t, Request> requested;
    bool stopping = false;

    std::mutex completionMutex;
    std::vector<WidgetFilesystemWatchCompletion> completions;
    std::jthread worker;

    std::unordered_map<std::uint64_t, std::unique_ptr<Watch>> watches;
    std::unordered_map<std::uint64_t, Request> restartRequests;
    bool workerStopping = false;

    Impl()
    {
        completionPort = CreateIoCompletionPort(
            INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (completionPort)
        {
            worker = std::jthread([this](std::stop_token) {
                WorkerMain();
            });
        }
    }

    ~Impl()
    {
        if (!completionPort) return;
        {
            std::scoped_lock lock(commandMutex);
            stopping = true;
            requested.clear();
            commands.push_back({ CommandKind::Shutdown });
        }
        PostQueuedCompletionStatus(completionPort, 0, 0, nullptr);
        if (worker.joinable()) worker.join();
        CloseHandle(completionPort);
        completionPort = nullptr;
    }

    void QueueCompletion(WidgetFilesystemWatchCompletion completion)
    {
        std::scoped_lock lock(completionMutex);
        if (completion.kind == WidgetFilesystemWatchCompletionKind::Events)
        {
            auto existing = completions.rend();
            for (auto candidate = completions.rbegin();
                candidate != completions.rend(); ++candidate)
            {
                if (candidate->id != completion.id) continue;
                if (candidate->kind ==
                    WidgetFilesystemWatchCompletionKind::Events)
                    existing = candidate;
                break;
            }
            if (existing != completions.rend())
            {
                existing->overflow = existing->overflow ||
                    completion.overflow;
                for (auto& event : completion.events)
                {
                    if (existing->events.size() >=
                        WidgetFilesystemWatchService::MaximumPendingEvents)
                    {
                        existing->overflow = true;
                        break;
                    }
                    const bool duplicate = std::any_of(
                        existing->events.begin(), existing->events.end(),
                        [&event](const auto& current) {
                            return current.kind == event.kind &&
                                current.name == event.name &&
                                current.oldName == event.oldName;
                        });
                    if (!duplicate)
                        existing->events.push_back(std::move(event));
                }
                return;
            }
        }
        completions.push_back(std::move(completion));
    }

    bool Arm(Watch& watch)
    {
        if (watch.pending || watch.stopping) return false;
        watch.overlapped = {};
        watch.pending = ReadDirectoryChangesW(
            watch.directory, watch.buffer.data(),
            static_cast<DWORD>(watch.buffer.size()), FALSE, kNotifyFilter,
            nullptr, &watch.overlapped, nullptr) != FALSE;
        return watch.pending;
    }

    void CloseAndErase(
        std::unordered_map<std::uint64_t,
            std::unique_ptr<Watch>>::iterator watch)
    {
        const std::uint64_t id = watch->first;
        if (watch->second->directory != INVALID_HANDLE_VALUE)
            CloseHandle(watch->second->directory);
        watches.erase(watch);
        QueueCompletion({ id,
            WidgetFilesystemWatchCompletionKind::Stopped });

        if (!workerStopping)
        {
            auto restart = restartRequests.find(id);
            if (restart != restartRequests.end())
            {
                Request request = std::move(restart->second);
                restartRequests.erase(restart);
                StartWatch(id, std::move(request));
            }
        }
    }

    void StopWatch(std::uint64_t id)
    {
        restartRequests.erase(id);
        auto watch = watches.find(id);
        if (watch == watches.end()) return;
        watch->second->stopping = true;
        if (watch->second->pending)
        {
            (void)CancelIoEx(
                watch->second->directory, &watch->second->overlapped);
            return;
        }
        CloseAndErase(watch);
    }

    void StartWatch(std::uint64_t id, Request request)
    {
        if (workerStopping) return;
        if (auto existing = watches.find(id); existing != watches.end())
        {
            restartRequests.insert_or_assign(id, std::move(request));
            existing->second->stopping = true;
            if (existing->second->pending)
                (void)CancelIoEx(existing->second->directory,
                    &existing->second->overlapped);
            else
                CloseAndErase(existing);
            return;
        }

        const DWORD attributes = GetFileAttributesW(
            request.directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            QueueCompletion({ id,
                WidgetFilesystemWatchCompletionKind::Error,
                {}, false, "invalidDirectory" });
            return;
        }

        auto watch = std::make_unique<Watch>();
        watch->id = id;
        watch->buffer.resize(kNotificationBufferBytes);
        watch->directory = CreateFileW(request.directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (!watch->directory || watch->directory == INVALID_HANDLE_VALUE)
        {
            QueueCompletion({ id,
                WidgetFilesystemWatchCompletionKind::Error,
                {}, false, "watchOpenFailed" });
            return;
        }

        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(watch->directory,
                FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !CreateIoCompletionPort(watch->directory, completionPort,
                static_cast<ULONG_PTR>(id), 0))
        {
            CloseHandle(watch->directory);
            QueueCompletion({ id,
                WidgetFilesystemWatchCompletionKind::Error,
                {}, false, "watchOpenFailed" });
            return;
        }

        Watch* raw = watch.get();
        watches.emplace(id, std::move(watch));
        if (!Arm(*raw))
        {
            auto failed = watches.find(id);
            if (failed != watches.end()) CloseAndErase(failed);
            QueueCompletion({ id,
                WidgetFilesystemWatchCompletionKind::Error,
                {}, false, "watchStartFailed" });
            return;
        }
        QueueCompletion({ id,
            WidgetFilesystemWatchCompletionKind::Started });
    }

    std::vector<WidgetFilesystemWatchEvent> Decode(
        Watch& watch, DWORD bytes)
    {
        std::vector<WidgetFilesystemWatchEvent> events;
        DWORD offset = 0;
        while (offset < bytes)
        {
            if (bytes - offset < sizeof(FILE_NOTIFY_INFORMATION)) break;
            const auto* info = reinterpret_cast<
                const FILE_NOTIFY_INFORMATION*>(watch.buffer.data() + offset);
            const DWORD fixedBytes = static_cast<DWORD>(
                offsetof(FILE_NOTIFY_INFORMATION, FileName));
            if (info->FileNameLength > bytes - offset - fixedBytes ||
                info->FileNameLength % sizeof(wchar_t) != 0)
                break;
            const std::wstring name(info->FileName,
                info->FileNameLength / sizeof(wchar_t));
            if (IsSafeDirectName(name))
            {
                if (info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                {
                    if (!watch.pendingRenameOld.empty())
                    {
                        events.push_back({ "removed",
                            std::move(watch.pendingRenameOld), {} });
                    }
                    watch.pendingRenameOld = name;
                }
                else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    if (!watch.pendingRenameOld.empty())
                    {
                        events.push_back({ "renamed", name,
                            std::move(watch.pendingRenameOld) });
                        watch.pendingRenameOld.clear();
                    }
                    else
                    {
                        events.push_back({ "added", name, {} });
                    }
                }
                else
                {
                    if (!watch.pendingRenameOld.empty())
                    {
                        events.push_back({ "removed",
                            std::move(watch.pendingRenameOld), {} });
                        watch.pendingRenameOld.clear();
                    }
                    if (info->Action == FILE_ACTION_ADDED)
                        events.push_back({ "added", name, {} });
                    else if (info->Action == FILE_ACTION_REMOVED)
                        events.push_back({ "removed", name, {} });
                    else if (info->Action == FILE_ACTION_MODIFIED)
                        events.push_back({ "modified", name, {} });
                }
            }
            if (info->NextEntryOffset == 0) break;
            if (info->NextEntryOffset > bytes - offset) break;
            offset += info->NextEntryOffset;
        }
        return events;
    }

    void ProcessCommands()
    {
        std::deque<Command> pending;
        {
            std::scoped_lock lock(commandMutex);
            pending.swap(commands);
        }
        for (auto& command : pending)
        {
            if (command.kind == CommandKind::Start)
                StartWatch(command.id, std::move(command.request));
            else if (command.kind == CommandKind::Stop)
                StopWatch(command.id);
            else
            {
                workerStopping = true;
                restartRequests.clear();
                std::vector<std::uint64_t> ids;
                ids.reserve(watches.size());
                for (const auto& [id, _] : watches) ids.push_back(id);
                for (const auto id : ids) StopWatch(id);
            }
        }
    }

    void WorkerMain()
    {
        for (;;)
        {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL succeeded = GetQueuedCompletionStatus(
                completionPort, &bytes, &key, &overlapped, INFINITE);
            if (!overlapped)
            {
                ProcessCommands();
                if (workerStopping && watches.empty()) break;
                continue;
            }

            const std::uint64_t id = static_cast<std::uint64_t>(key);
            auto watch = watches.find(id);
            if (watch == watches.end() ||
                &watch->second->overlapped != overlapped)
                continue;
            watch->second->pending = false;
            const DWORD completionError = succeeded
                ? ERROR_SUCCESS : GetLastError();
            if (watch->second->stopping ||
                completionError == ERROR_OPERATION_ABORTED)
            {
                CloseAndErase(watch);
                if (workerStopping && watches.empty()) break;
                continue;
            }

            WidgetFilesystemWatchCompletion completion;
            completion.id = id;
            completion.kind = WidgetFilesystemWatchCompletionKind::Events;
            completion.overflow =
                (!succeeded && completionError == ERROR_NOTIFY_ENUM_DIR) ||
                (succeeded && bytes == 0);
            if (succeeded && bytes != 0)
                completion.events = Decode(*watch->second, bytes);

            if (!succeeded && !completion.overflow)
            {
                completion.kind = WidgetFilesystemWatchCompletionKind::Error;
                completion.error = "watchReadFailed";
                watch->second->stopping = true;
                CloseAndErase(watch);
            }
            else
            {
                if (!Arm(*watch->second))
                {
                    completion.kind =
                        WidgetFilesystemWatchCompletionKind::Error;
                    completion.error = "watchRestartFailed";
                    watch->second->stopping = true;
                    CloseAndErase(watch);
                }
            }
            QueueCompletion(std::move(completion));
            if (workerStopping && watches.empty()) break;
        }
    }
};

WidgetFilesystemWatchService::WidgetFilesystemWatchService()
    : impl_(std::make_unique<Impl>())
{
}

WidgetFilesystemWatchService::~WidgetFilesystemWatchService() = default;

WidgetFilesystemWatchStartResult WidgetFilesystemWatchService::Start(
    std::uint64_t id, std::string instanceId,
    std::filesystem::path directory)
{
    if (!impl_ || !impl_->completionPort)
        return { false, "providerUnavailable" };
    if (id == 0 || instanceId.empty() || directory.empty())
        return { false, "invalidArguments" };
    {
        std::scoped_lock lock(impl_->commandMutex);
        if (impl_->stopping)
            return { false, "providerUnavailable" };
        Impl::Request request{ std::move(instanceId),
            std::move(directory) };
        impl_->requested.insert_or_assign(id, request);
        impl_->commands.push_back(
            { Impl::CommandKind::Start, id, std::move(request) });
    }
    if (!PostQueuedCompletionStatus(
            impl_->completionPort, 0, 0, nullptr))
        return { false, "providerUnavailable" };
    return { true, {} };
}

bool WidgetFilesystemWatchService::Stop(std::uint64_t id)
{
    if (!impl_ || !impl_->completionPort || id == 0) return false;
    {
        std::scoped_lock lock(impl_->commandMutex);
        if (impl_->stopping || impl_->requested.erase(id) == 0)
            return false;
        impl_->commands.push_back({ Impl::CommandKind::Stop, id });
    }
    (void)PostQueuedCompletionStatus(
        impl_->completionPort, 0, 0, nullptr);
    return true;
}

std::size_t WidgetFilesystemWatchService::ForgetInstance(
    std::string_view instanceId)
{
    if (!impl_ || !impl_->completionPort || instanceId.empty()) return 0;
    std::size_t removed = 0;
    {
        std::scoped_lock lock(impl_->commandMutex);
        if (impl_->stopping) return 0;
        for (auto request = impl_->requested.begin();
            request != impl_->requested.end();)
        {
            if (request->second.instanceId != instanceId)
            {
                ++request;
                continue;
            }
            impl_->commands.push_back(
                { Impl::CommandKind::Stop, request->first });
            request = impl_->requested.erase(request);
            ++removed;
        }
    }
    if (removed != 0)
        (void)PostQueuedCompletionStatus(
            impl_->completionPort, 0, 0, nullptr);
    return removed;
}

std::vector<WidgetFilesystemWatchCompletion>
WidgetFilesystemWatchService::DrainCompletions()
{
    if (!impl_) return {};
    std::scoped_lock lock(impl_->completionMutex);
    return std::exchange(impl_->completions, {});
}

std::size_t WidgetFilesystemWatchService::RequestedCount() const
{
    if (!impl_) return 0;
    std::scoped_lock lock(impl_->commandMutex);
    return impl_->requested.size();
}
}
