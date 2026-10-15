#pragma once

#include <windows.h>

#include <array>
#include <memory>
#include <string>

namespace snowdesktop::shell
{

// The kernel retains both OVERLAPPED and buffer addresses until completion.
// This object is deliberately immovable; containers own it through unique_ptr.
class DirectoryChangeWatch
{
public:
    DirectoryChangeWatch(const DirectoryChangeWatch&) = delete;
    DirectoryChangeWatch& operator=(const DirectoryChangeWatch&) = delete;
    ~DirectoryChangeWatch()
    {
        CancelAndDrain();
        if (event_) CloseHandle(event_);
        if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
    }

    static std::unique_ptr<DirectoryChangeWatch> Create(const std::wstring& path)
    {
        auto watch = std::unique_ptr<DirectoryChangeWatch>(new DirectoryChangeWatch);
        watch->directory_ = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (watch->directory_ == INVALID_HANDLE_VALUE)
            return {};
        watch->event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!watch->event_ || !watch->Arm())
            return {};
        return watch;
    }

    HANDLE Event() const noexcept { return event_; }

    bool Complete(DWORD& bytes) noexcept
    {
        if (!pending_)
            return false;
        const BOOL result = GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE);
        if (result || GetLastError() != ERROR_IO_INCOMPLETE)
            pending_ = false;
        return result != FALSE;
    }

    bool Arm() noexcept
    {
        if (pending_)
            return false;
        ResetEvent(event_);
        overlapped_ = {};
        overlapped_.hEvent = event_;
        pending_ = ReadDirectoryChangesW(directory_, buffer_.data(),
            static_cast<DWORD>(buffer_.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &overlapped_, nullptr) != FALSE;
        return pending_;
    }

    void CancelAndDrain() noexcept
    {
        if (!pending_)
            return;
        CancelIoEx(directory_, &overlapped_);
        DWORD bytes = 0;
        // Cancellation is only a request; keep both buffers and the event
        // alive until Windows acknowledges completion (including aborted I/O).
        GetOverlappedResult(directory_, &overlapped_, &bytes, TRUE);
        pending_ = false;
    }

private:
    DirectoryChangeWatch() = default;
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED overlapped_{};
    alignas(DWORD) std::array<BYTE, 64 * 1024> buffer_{};
    bool pending_ = false;
};

} // namespace snowdesktop::shell
