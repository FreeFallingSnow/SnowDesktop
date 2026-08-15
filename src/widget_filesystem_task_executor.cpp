#include "widget_filesystem_task_executor.h"

#include "atomic_file.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::uint64_t kWindowsToUnixEpochTicks =
    116444736000000000ULL;

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), result.data(),
            size, nullptr, nullptr) != size)
        return {};
    return result;
}

bool IsValidUtf8(std::string_view value)
{
    if (value.empty()) return true;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool CheckPathWithoutReparsePoints(const std::filesystem::path& path,
    bool allowMissingLeaf, std::string& error)
{
    error.clear();
    if (path.empty() || !path.is_absolute())
    {
        error = "invalidReference";
        return false;
    }
    const auto normalized = path.lexically_normal();
    std::filesystem::path current = normalized.root_path();
    for (const auto& component : normalized.relative_path())
    {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if (allowMissingLeaf && current == normalized &&
                (code == ERROR_FILE_NOT_FOUND ||
                    code == ERROR_PATH_NOT_FOUND))
                return true;
            error = code == ERROR_ACCESS_DENIED
                ? "accessDenied" : "notFound";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            error = "reparsePointDenied";
            return false;
        }
    }
    return true;
}

std::string MakeRevision(std::uint64_t size, const FILETIME& modified,
    WidgetFilesystemHandleKind kind)
{
    ULARGE_INTEGER timestamp{};
    timestamp.LowPart = modified.dwLowDateTime;
    timestamp.HighPart = modified.dwHighDateTime;
    std::ostringstream output;
    output << "r1-" << std::hex << std::setw(16) << std::setfill('0')
        << timestamp.QuadPart << '-' << std::setw(16) << size << '-'
        << (kind == WidgetFilesystemHandleKind::Folder ? 'd' : 'f');
    return output.str();
}

bool ReadMetadata(const std::filesystem::path& path,
    WidgetFilesystemMetadata& metadata, std::string& error)
{
    error.clear();
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
            &attributes))
    {
        error = GetLastError() == ERROR_ACCESS_DENIED
            ? "accessDenied" : "notFound";
        return false;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        error = "reparsePointDenied";
        return false;
    }
    const bool folder = (attributes.dwFileAttributes &
        FILE_ATTRIBUTE_DIRECTORY) != 0;
    metadata = {};
    metadata.kind = folder ? WidgetFilesystemHandleKind::Folder
                           : WidgetFilesystemHandleKind::File;
    metadata.path = path;
    metadata.name = WideToUtf8(path.filename().wstring());
    if (metadata.name.empty() && !path.filename().empty())
    {
        error = "invalidName";
        return false;
    }
    metadata.size = folder ? 0 :
        (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
            attributes.nFileSizeLow;
    ULARGE_INTEGER timestamp{};
    timestamp.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    timestamp.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    metadata.modifiedMs = timestamp.QuadPart >= kWindowsToUnixEpochTicks
        ? static_cast<std::int64_t>((timestamp.QuadPart -
            kWindowsToUnixEpochTicks) / 10000ULL)
        : 0;
    metadata.readOnly = (attributes.dwFileAttributes &
        FILE_ATTRIBUTE_READONLY) != 0;
    metadata.revision = MakeRevision(
        metadata.size, attributes.ftLastWriteTime, metadata.kind);
    return true;
}

WidgetFilesystemTaskRunResult RunStat(
    const WidgetFilesystemTaskRequest& request)
{
    std::string error;
    if (!CheckPathWithoutReparsePoints(request.path, false, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    WidgetFilesystemMetadata metadata;
    if (!ReadMetadata(request.path, metadata, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    WidgetFilesystemTaskRunResult result;
    result.ok = true;
    result.metadata = std::move(metadata);
    return result;
}

WidgetFilesystemTaskRunResult RunList(
    const WidgetFilesystemTaskRequest& request)
{
    std::string error;
    if (!CheckPathWithoutReparsePoints(request.path, false, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    WidgetFilesystemMetadata folder;
    if (!ReadMetadata(request.path, folder, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    if (folder.kind != WidgetFilesystemHandleKind::Folder)
        return { false, {}, {}, {}, 0, false, "notFolder" };

    std::vector<WidgetFilesystemMetadata> entries;
    std::error_code filesystemError;
    for (std::filesystem::directory_iterator iterator(request.path,
            std::filesystem::directory_options::none, filesystemError), end;
        !filesystemError && iterator != end;
        iterator.increment(filesystemError))
    {
        if (entries.size() >=
            WidgetFilesystemTaskExecutor::MaximumDirectoryEntries)
            return { false, {}, {}, {}, 0, false,
                "directoryTooLarge" };
        WidgetFilesystemMetadata entry;
        std::string metadataError;
        if (!ReadMetadata(iterator->path(), entry, metadataError))
        {
            if (metadataError == "reparsePointDenied" ||
                metadataError == "notFound")
                continue;
            return { false, {}, {}, {}, 0, false,
                std::move(metadataError) };
        }
        entries.push_back(std::move(entry));
    }
    if (filesystemError)
        return { false, {}, {}, {}, 0, false, "listFailed" };
    std::sort(entries.begin(), entries.end(), [](const auto& left,
        const auto& right) {
            return left.path.filename().wstring() <
                right.path.filename().wstring();
        });

    WidgetFilesystemTaskRunResult result;
    result.ok = true;
    result.metadata = std::move(folder);
    if (request.offset < entries.size())
    {
        const std::size_t end = std::min(entries.size(),
            request.offset + request.limit);
        result.items.insert(result.items.end(),
            std::make_move_iterator(entries.begin() +
                static_cast<std::ptrdiff_t>(request.offset)),
            std::make_move_iterator(entries.begin() +
                static_cast<std::ptrdiff_t>(end)));
        result.nextOffset = end;
        result.hasMore = end < entries.size();
    }
    else
    {
        result.nextOffset = request.offset;
    }
    return result;
}

WidgetFilesystemTaskRunResult RunRead(
    const WidgetFilesystemTaskRequest& request)
{
    std::string error;
    if (!CheckPathWithoutReparsePoints(request.path, false, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    WidgetFilesystemMetadata metadata;
    if (!ReadMetadata(request.path, metadata, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    if (metadata.kind != WidgetFilesystemHandleKind::File)
        return { false, {}, {}, {}, 0, false, "notFile" };
    if (metadata.size > request.maxBytes ||
        metadata.size > WidgetFilesystemTaskExecutor::MaximumTextBytes)
        return { false, {}, {}, {}, 0, false, "fileTooLarge" };

    std::ifstream file(request.path, std::ios::binary);
    if (!file)
        return { false, {}, {}, {}, 0, false, "readFailed" };
    std::string text;
    text.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    if (file.bad())
        return { false, {}, {}, {}, 0, false, "readFailed" };
    if (text.size() != metadata.size)
        return { false, {}, {}, {}, 0, false, "fileChanged" };
    WidgetFilesystemMetadata after;
    if (!ReadMetadata(request.path, after, error) ||
        after.revision != metadata.revision)
        return { false, {}, {}, {}, 0, false, "fileChanged" };
    if (text.find('\0') != std::string::npos || !IsValidUtf8(text))
        return { false, {}, {}, {}, 0, false, "invalidEncoding" };

    WidgetFilesystemTaskRunResult result;
    result.ok = true;
    result.metadata = std::move(after);
    result.text = std::move(text);
    return result;
}

WidgetFilesystemTaskRunResult RunWrite(
    const WidgetFilesystemTaskRequest& request)
{
    std::string error;
    if (!CheckPathWithoutReparsePoints(request.path, true, error))
        return { false, {}, {}, {}, 0, false, std::move(error) };
    WidgetFilesystemMetadata before;
    const DWORD existingAttributes = GetFileAttributesW(
        request.path.c_str());
    const bool exists = existingAttributes != INVALID_FILE_ATTRIBUTES;
    if (exists)
    {
        if (!ReadMetadata(request.path, before, error))
            return { false, {}, {}, {}, 0, false, std::move(error) };
        if (before.kind != WidgetFilesystemHandleKind::File)
            return { false, {}, {}, {}, 0, false, "notFile" };
    }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND &&
        GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return { false, {}, {}, {}, 0, false, "writeFailed" };
    }
    if (!request.expectedRevision.empty() &&
        (!exists || before.revision != request.expectedRevision))
        return { false, {}, {}, {}, 0, false, "conflict" };

    if (!atomic_file::WriteAll(request.path, request.text, {}, &error))
        return { false, {}, {}, {}, 0, false, "writeFailed" };
    WidgetFilesystemMetadata after;
    if (!ReadMetadata(request.path, after, error))
        return { false, {}, {}, {}, 0, false, "writeVerificationFailed" };
    WidgetFilesystemTaskRunResult result;
    result.ok = true;
    result.metadata = std::move(after);
    return result;
}
}

WidgetFilesystemTaskExecutor::WidgetFilesystemTaskExecutor(
    Runner runner, NowProvider nowProvider)
    : runner_(std::move(runner)), nowProvider_(std::move(nowProvider))
{
    if (!runner_) runner_ = RunSystemAction;
    if (!nowProvider_)
        nowProvider_ = [] { return Clock::now(); };
}

WidgetFilesystemTaskExecutor::~WidgetFilesystemTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

WidgetFilesystemTaskStartResult WidgetFilesystemTaskExecutor::Start(
    std::uint64_t id, std::string instanceId,
    WidgetFilesystemTaskRequest request)
{
    if (id == 0 || instanceId.empty() || !ValidateRequest(request))
        return { false, "invalidArguments" };
    const auto now = nowProvider_();
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id))
        return { false, "taskExecutorUnavailable" };
    if (request.action == "filesystem.write")
    {
        if (const auto last = lastWrites_.find(instanceId);
            last != lastWrites_.end() && now >= last->second &&
            now - last->second < MinimumWriteInterval)
            return { false, "rateLimited" };
        lastWrites_.insert_or_assign(instanceId, now);
    }
    active_.insert(id);
    requests_.push_back(
        { id, std::move(instanceId), std::move(request) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return { true, {} };
}

bool WidgetFilesystemTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (!active_.contains(id)) return false;
    canceled_.insert(id);
    condition_.notify_all();
    return true;
}

void WidgetFilesystemTaskExecutor::ForgetInstance(
    std::string_view instanceId)
{
    std::scoped_lock lock(mutex_);
    lastWrites_.erase(std::string(instanceId));
}

std::vector<WidgetFilesystemTaskCompletion>
WidgetFilesystemTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetFilesystemTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

bool WidgetFilesystemTaskExecutor::SupportsAction(
    std::string_view action) noexcept
{
    return action == "filesystem.stat" || action == "filesystem.list" ||
        action == "filesystem.read" || action == "filesystem.write";
}

bool WidgetFilesystemTaskExecutor::ValidateRequest(
    const WidgetFilesystemTaskRequest& request) noexcept
{
    if (!SupportsAction(request.action) || request.path.empty() ||
        !request.path.is_absolute())
        return false;
    if (request.action == "filesystem.stat")
        return request.text.empty() && request.expectedRevision.empty();
    if (request.action == "filesystem.list")
        return request.text.empty() && request.expectedRevision.empty() &&
            request.limit >= 1 && request.limit <= MaximumListLimit &&
            request.offset <= MaximumListOffset;
    if (request.action == "filesystem.read")
        return request.text.empty() && request.expectedRevision.empty() &&
            request.maxBytes >= 1 && request.maxBytes <= MaximumTextBytes;
    return request.text.size() <= MaximumTextBytes &&
        request.text.find('\0') == std::string::npos &&
        IsValidUtf8(request.text) && request.expectedRevision.size() <= 64;
}

WidgetFilesystemTaskRunResult
WidgetFilesystemTaskExecutor::RunSystemAction(
    const WidgetFilesystemTaskRequest& request)
{
    if (!ValidateRequest(request))
        return { false, {}, {}, {}, 0, false, "invalidArguments" };
    WidgetFilesystemTaskRunResult result;
    if (request.action == "filesystem.stat") result = RunStat(request);
    else if (request.action == "filesystem.list") result = RunList(request);
    else if (request.action == "filesystem.read") result = RunRead(request);
    else result = RunWrite(request);
    if (result.ok) result.metadata.handle = request.handle;
    return result;
}

void WidgetFilesystemTaskExecutor::WorkerMain(
    std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        QueuedRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.contains(request.id))
            {
                active_.erase(request.id);
                canceled_.erase(request.id);
                completions_.push_back({ request.id,
                    request.request.action, false, {}, {}, {}, 0,
                    false, "canceled" });
                continue;
            }
        }

        WidgetFilesystemTaskRunResult result;
        try
        {
            result = runner_(request.request);
        }
        catch (...)
        {
            result = { false, {}, {}, {}, 0, false,
                "filesystemTaskFailed" };
        }
        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
                result = { false, {}, {}, {}, 0, false, "canceled" };
            active_.erase(request.id);
            completions_.push_back({ request.id,
                std::move(request.request.action), result.ok,
                std::move(result.metadata), std::move(result.items),
                std::move(result.text), result.nextOffset,
                result.hasMore, std::move(result.error) });
        }
    }
}
}
