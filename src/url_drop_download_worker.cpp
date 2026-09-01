#include "url_drop_download_worker.h"

#include "http_runtime.h"
#include "url_drop_resource.h"

#include <windows.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cctype>
#include <cwctype>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop
{
namespace
{

std::wstring LowerTrimmed(std::wstring value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1])) --last;
    value = value.substr(first, last - first);
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    return value;
}

bool IsValidDestinationDirectory(const std::filesystem::path& directory)
{
    if (directory.empty()) return false;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

class ScopedFileHandle final
{
public:
    ~ScopedFileHandle()
    {
        Reset();
    }

    ScopedFileHandle() = default;
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    void Reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }

    HANDLE Get() const noexcept
    {
        return value_;
    }

    explicit operator bool() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class StagedOutputGuard final
{
public:
    explicit StagedOutputGuard(const std::filesystem::path& path)
        : path_(path)
    {
    }

    ~StagedOutputGuard()
    {
        if (!keep_ && !path_.empty())
            DeleteFileW(path_.c_str());
    }

    StagedOutputGuard(const StagedOutputGuard&) = delete;
    StagedOutputGuard& operator=(const StagedOutputGuard&) = delete;

    void Keep() noexcept
    {
        keep_ = true;
    }

private:
    const std::filesystem::path& path_;
    bool keep_ = false;
};

bool IsOrdinaryNonEmptyFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;

    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool valid = GetFileInformationByHandle(file, &information) &&
        (information.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        (information.nFileSizeHigh != 0 || information.nFileSizeLow != 0);
    CloseHandle(file);
    return valid;
}

bool HasDangerousShellAssociation(const std::wstring& fileName)
{
    const wchar_t* extension = PathFindExtensionW(fileName.c_str());
    return extension && *extension && AssocIsDangerous(extension);
}

HANDLE CreateUniqueOutputFile(const std::filesystem::path& directory,
    const std::wstring& suggestedFileName,
    std::filesystem::path& outputPath)
{
    const std::filesystem::path original(
        suggestedFileName.empty() ? L"download.bin" : suggestedFileName);
    std::wstring stem = original.stem().wstring();
    const std::wstring extension = original.extension().wstring();
    if (stem.empty()) stem = L"download";
    for (unsigned int suffix = 0; suffix < 10000; ++suffix)
    {
        const std::wstring fileName = suffix == 0
            ? original.filename().wstring()
            : stem + L" (" + std::to_wstring(suffix) + L")" + extension;
        std::filesystem::path candidate = directory / fileName;
        HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            outputPath = std::move(candidate);
            return file;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    return INVALID_HANDLE_VALUE;
}

bool LooksLikeHtml(std::span<const std::byte> prefix)
{
    size_t index = 0;
    if (prefix.size() >= 3 &&
        prefix[0] == std::byte{0xef} &&
        prefix[1] == std::byte{0xbb} &&
        prefix[2] == std::byte{0xbf})
        index = 3;
    while (index < prefix.size())
    {
        const unsigned char character =
            std::to_integer<unsigned char>(prefix[index]);
        if (!std::isspace(character)) break;
        ++index;
    }
    std::string sample;
    const size_t sampleSize = std::min<size_t>(
        prefix.size() - index, 512);
    sample.reserve(sampleSize);
    for (size_t offset = 0; offset < sampleSize; ++offset)
    {
        const unsigned char character = std::to_integer<unsigned char>(
            prefix[index + offset]);
        sample.push_back(static_cast<char>(std::tolower(character)));
    }
    size_t cursor = 0;
    const auto skipWhitespace = [&]() {
        while (cursor < sample.size() &&
            std::isspace(static_cast<unsigned char>(sample[cursor])))
            ++cursor;
    };
    for (int prefixCount = 0; prefixCount < 4; ++prefixCount)
    {
        skipWhitespace();
        if (sample.compare(cursor, 4, "<!--") == 0)
        {
            const size_t end = sample.find("-->", cursor + 4);
            if (end == std::string::npos) return false;
            cursor = end + 3;
            continue;
        }
        if (sample.compare(cursor, 5, "<?xml") == 0)
        {
            const size_t end = sample.find("?>", cursor + 5);
            if (end == std::string::npos) return false;
            cursor = end + 2;
            continue;
        }
        break;
    }
    skipWhitespace();
    const std::string_view document(sample.data() + cursor,
        sample.size() - cursor);
    return document.starts_with("<!doctype html") ||
        document.starts_with("<html") ||
        document.starts_with("<head") ||
        document.starts_with("<body") ||
        document.starts_with("<meta") ||
        document.starts_with("<title");
}

bool MarkDownloadedAttachment(const std::filesystem::path& path,
    const std::wstring& sourceUrl, const std::wstring& referrerUrl)
{
    Microsoft::WRL::ComPtr<IAttachmentExecute> attachment;
    if (FAILED(CoCreateInstance(CLSID_AttachmentServices, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&attachment))) ||
        !attachment)
        return false;
    if (FAILED(attachment->SetLocalPath(path.c_str())) ||
        FAILED(attachment->SetSource(sourceUrl.c_str())))
        return false;
    if (!referrerUrl.empty() &&
        _wcsicmp(sourceUrl.c_str(), referrerUrl.c_str()) != 0)
        (void)attachment->SetReferrer(referrerUrl.c_str());
    return SUCCEEDED(attachment->Save());
}

UrlDropDownloadResult CancelledResult(const UrlDropDownloadRequest& request)
{
    UrlDropDownloadResult result;
    result.originalUrl = request.url;
    result.error = "Cancelled";
    return result;
}

} // namespace

UrlDropDownloadWorker::~UrlDropDownloadWorker()
{
    Stop();
}

bool UrlDropDownloadWorker::Enqueue(
    UrlDropDownloadRequest request, Completion completion)
{
    if (request.url.empty() || request.destinationDirectory.empty() ||
        !completion)
        return false;

    Task task{ std::move(request), std::move(completion) };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= 8)
            return false;
        try
        {
            tasks_.push_back(std::move(task));
        }
        catch (...)
        {
            return false;
        }
        if (!started_)
        {
            try
            {
                thread_ = std::thread(&UrlDropDownloadWorker::Run, this);
                started_ = true;
            }
            catch (...)
            {
                tasks_.pop_back();
                return false;
            }
        }
    }
    cv_.notify_one();
    return true;
}

void UrlDropDownloadWorker::Stop()
{
    std::deque<Task> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        activeStopSource_.request_stop();
        cancelled.swap(tasks_);
    }
    for (auto& task : cancelled)
        if (task.completion)
            task.completion(CancelledResult(task.request));
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

UrlDropDownloadResult UrlDropDownloadWorker::Execute(
    const UrlDropDownloadRequest& request, std::stop_token token)
{
    UrlDropDownloadResult result;
    result.originalUrl = request.url;
    if (!IsValidDestinationDirectory(request.destinationDirectory))
    {
        result.error = "Invalid drop-content directory";
        return result;
    }

    std::filesystem::path outputPath;
    StagedOutputGuard outputGuard(outputPath);
    ScopedFileHandle outputFile;
    bool rejectedAsShortcut = false;
    bool outputCreationFailed = false;
    std::vector<std::byte> prefix;
    prefix.reserve(512);

    http_stream::Options options;
    options.url = request.url;
    options.timeoutMs = request.timeoutMs;
    options.totalTimeoutMs = 60000;
    options.maximumResponseBytes = request.maximumBytes;
    options.maxRedirects = 3;
    const auto streamResult = http_stream::StreamHttpGet(
        options, token,
        [&](const http_stream::ResponseHead& head) {
            result.finalUrl = head.finalUrl;
            result.contentType = head.contentType;
            const std::wstring encoding = LowerTrimmed(head.contentEncoding);
            if (!encoding.empty() && encoding != L"identity")
            {
                rejectedAsShortcut = true;
                return false;
            }
            const auto decision = url_drop_resource::Decide(
                head.finalUrl, head.contentType, head.contentDisposition);
            if (decision.action == url_drop_resource::Action::Shortcut)
            {
                rejectedAsShortcut = true;
                return false;
            }
            if (HasDangerousShellAssociation(
                    decision.suggestedFileName))
            {
                rejectedAsShortcut = true;
                return false;
            }
            outputFile.Reset(CreateUniqueOutputFile(
                request.destinationDirectory,
                decision.suggestedFileName, outputPath));
            if (!outputFile)
            {
                outputCreationFailed = true;
                return false;
            }
            return true;
        },
        [&](std::span<const std::byte> chunk) {
            if (!outputFile)
                return false;
            if (prefix.size() < 512)
            {
                const size_t copySize = std::min<size_t>(
                    512 - prefix.size(), chunk.size());
                prefix.insert(prefix.end(), chunk.begin(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(copySize));
            }
            DWORD written = 0;
            return chunk.size() <= MAXDWORD &&
                WriteFile(outputFile.Get(), chunk.data(),
                    static_cast<DWORD>(chunk.size()), &written, nullptr) &&
                written == chunk.size();
        });

    bool flushed = true;
    if (outputFile)
    {
        flushed = FlushFileBuffers(outputFile.Get()) != FALSE;
        outputFile.Reset();
    }

    if (token.stop_requested() || streamResult.cancelled)
    {
        if (!outputPath.empty()) DeleteFileW(outputPath.c_str());
        result.error = "Cancelled";
        return result;
    }
    if (rejectedAsShortcut)
    {
        if (!outputPath.empty()) DeleteFileW(outputPath.c_str());
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }
    if (outputCreationFailed)
    {
        result.error = "Cannot create staged download";
        return result;
    }
    if (!flushed)
    {
        result.error = "Cannot flush staged download";
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }
    if (!streamResult.error.empty() ||
        !streamResult.responseAccepted ||
        streamResult.bytesReceived == 0)
    {
        if (!outputPath.empty()) DeleteFileW(outputPath.c_str());
        result.error = streamResult.error.empty()
            ? "Empty resource response" : streamResult.error;
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }
    if (LooksLikeHtml(prefix))
    {
        DeleteFileW(outputPath.c_str());
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }
    if (!MarkDownloadedAttachment(outputPath,
            result.finalUrl.empty() ? request.url : result.finalUrl,
            request.url))
    {
        DeleteFileW(outputPath.c_str());
        result.error = "Attachment policy rejected the download";
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }
    if (token.stop_requested())
    {
        DeleteFileW(outputPath.c_str());
        result.error = "Cancelled";
        return result;
    }
    if (!IsOrdinaryNonEmptyFile(outputPath))
    {
        DeleteFileW(outputPath.c_str());
        result.error = "Attachment policy removed or changed the download";
        result.outcome = UrlDropDownloadOutcome::Shortcut;
        return result;
    }

    result.outcome = UrlDropDownloadOutcome::Downloaded;
    result.localPath = outputPath.wstring();
    outputGuard.Keep();
    return result;
}

void UrlDropDownloadWorker::Run()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;)
    {
        Task task;
        std::stop_source taskStopSource;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
            taskStopSource = std::stop_source{};
            activeStopSource_ = taskStopSource;
            executing_ = true;
        }

        UrlDropDownloadResult result;
        try
        {
            result = Execute(
                task.request, taskStopSource.get_token());
        }
        catch (...)
        {
            result.originalUrl = task.request.url;
            result.error = "Unexpected download worker failure";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            executing_ = false;
            activeStopSource_ = std::stop_source{};
        }
        if (task.completion)
        {
            try
            {
                task.completion(std::move(result));
            }
            catch (...)
            {
            }
        }
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
}

} // namespace snowdesktop
