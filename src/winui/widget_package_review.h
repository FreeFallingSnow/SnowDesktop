#pragma once

#include "windows_compat.h"
#include "widgets_page_backend_state.h"
#include "widget/packages/widget_package.h"
#include "widget/packages/widget_package_read.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace snowdesktop::winui::widgets_page_backend_detail
{
// Internal settings installation policy, shared with the filesystem regression
// tests. A review lock is an optional protection, not proof that IO will fail.
class ScopedPackageIdentityLock final
{
public:
    explicit ScopedPackageIdentityLock(const std::filesystem::path& path,
        const std::filesystem::path& trustedRoot,
        const std::optional<ReviewedPackageFileIdentity>& expected = {},
        bool directory = false) : directory_(directory)
    {
        std::error_code error;
        path_ = std::filesystem::absolute(path, error).lexically_normal();
        if (error) { Fail(L"resolve-path", path, static_cast<DWORD>(error.value())); return; }
        const auto root = std::filesystem::absolute(trustedRoot, error).lexically_normal();
        if (error || path_.parent_path() != root)
        {
            Fail(L"review-root", path_, error ? static_cast<DWORD>(error.value()) : ERROR_INVALID_NAME);
            return;
        }
        std::vector<std::filesystem::path> ancestors;
        for (auto current = path_.parent_path(); !current.empty();)
        {
            ancestors.push_back(current);
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        std::reverse(ancestors.begin(), ancestors.end());
        for (const auto& ancestor : ancestors)
        {
            HANDLE handle = CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            const DWORD failure = DiskObjectError(handle, true);
            if (failure != ERROR_SUCCESS)
            {
                if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
                Fail(L"lock-ancestor", ancestor, failure);
                Reset();
                return;
            }
            ancestorHandles_.emplace_back(ancestor, handle);
        }
        handle_ = OpenPath();
        DWORD failure = DiskObjectError(handle_, directory_);
        if (failure == ERROR_SUCCESS) failure = ReadIdentity(handle_, identity_);
        if (failure == ERROR_SUCCESS && expected && identity_ != *expected)
            failure = ERROR_FILE_INVALID;
        if (failure != ERROR_SUCCESS)
        {
            Fail(L"lock-package", path_, failure);
            Reset();
        }
    }

    ~ScopedPackageIdentityLock() { Reset(); }
    ScopedPackageIdentityLock(const ScopedPackageIdentityLock&) = delete;
    ScopedPackageIdentityLock& operator=(const ScopedPackageIdentityLock&) = delete;

    [[nodiscard]] bool Acquired() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] const ReviewedPackageFileIdentity& Identity() const noexcept { return identity_; }
    [[nodiscard]] DWORD ErrorCode() const noexcept { return errorCode_; }
    [[nodiscard]] const std::filesystem::path& ErrorPath() const noexcept { return errorPath_; }

    [[nodiscard]] std::wstring Details() const
    {
        return std::wstring(stage_) + L"\n" + errorPath_.wstring() +
            L"\nWin32=" + std::to_wstring(errorCode_);
    }

    [[nodiscard]] bool MatchesPathIdentity()
    {
        if (!Acquired()) return false;
        for (const auto& [path, handle] : ancestorHandles_)
        {
            const DWORD failure = DiskObjectError(handle, true);
            if (failure != ERROR_SUCCESS)
            {
                Fail(L"recheck-ancestor", path, failure);
                return false;
            }
        }
        ReviewedPackageFileIdentity held;
        DWORD failure = ReadIdentity(handle_, held);
        if (failure == ERROR_SUCCESS && held != identity_) failure = ERROR_FILE_INVALID;
        if (failure == ERROR_SUCCESS)
        {
            HANDLE reopened = OpenPath();
            failure = DiskObjectError(reopened, directory_);
            ReviewedPackageFileIdentity current;
            if (failure == ERROR_SUCCESS) failure = ReadIdentity(reopened, current);
            if (failure == ERROR_SUCCESS && current != identity_) failure = ERROR_FILE_INVALID;
            if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
        }
        if (failure != ERROR_SUCCESS) Fail(L"recheck-package", path_, failure);
        return failure == ERROR_SUCCESS;
    }

private:
    static DWORD DiskObjectError(HANDLE handle, bool directory) noexcept
    {
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        if (GetFileType(handle) != FILE_TYPE_DISK) return ERROR_NOT_SUPPORTED;
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
            return GetLastError();
        if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
            return ERROR_NOT_SUPPORTED;
        return ERROR_SUCCESS;
    }

    static DWORD ReadIdentity(HANDLE handle, ReviewedPackageFileIdentity& identity) noexcept
    {
        FILE_ID_INFO info{};
        if (GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
        {
            identity.volumeSerialNumber = info.VolumeSerialNumber;
            std::memcpy(identity.fileId.data(), info.FileId.Identifier, identity.fileId.size());
            return ERROR_SUCCESS;
        }
        BY_HANDLE_FILE_INFORMATION fallback{};
        if (!GetFileInformationByHandle(handle, &fallback)) return GetLastError();
        identity = {};
        identity.volumeSerialNumber = fallback.dwVolumeSerialNumber;
        const std::uint64_t index = (static_cast<std::uint64_t>(fallback.nFileIndexHigh) << 32) |
            fallback.nFileIndexLow;
        std::memcpy(identity.fileId.data(), &index, sizeof(index));
        return index == 0 ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
    }

    HANDLE OpenPath() const noexcept
    {
        return CreateFileW(path_.c_str(), directory_ ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                (directory_ ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
    }

    void Fail(const wchar_t* stage, const std::filesystem::path& path, DWORD error)
    {
        stage_ = stage;
        errorPath_ = path;
        errorCode_ = error;
    }

    void Reset() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        for (const auto& ancestor : ancestorHandles_) CloseHandle(ancestor.second);
        ancestorHandles_.clear();
    }

    std::filesystem::path path_, errorPath_;
    bool directory_ = false;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::vector<std::pair<std::filesystem::path, HANDLE>> ancestorHandles_;
    ReviewedPackageFileIdentity identity_;
    const wchar_t* stage_ = L"review-lock";
    DWORD errorCode_ = ERROR_SUCCESS;
};

enum class ReviewedPackageInstallStatus
{
    Installed,
    NeedsLockConfirmation,
    Changed,
    Unreadable,
    Invalid,
    InstallFailed,
};

struct ReviewedPackageInstallResult
{
    ReviewedPackageInstallStatus status;
    std::string validationDetails;
    std::wstring lockDetails;
};

// The operation is the real engine install in production and the real package
// manager in tests. Consent relaxes locking only; failed IO and changed content
// never become success, and the underlying installer still decides its result.
template<class InstallOperation>
ReviewedPackageInstallResult InstallReviewedPackage(
    const std::filesystem::path& path, const widget::PackagePaths& paths,
    const std::optional<ReviewedPackageFileIdentity>& identity,
    const std::string& sha256, const widget::PackageManifest& manifest,
    bool allowUnlockedReview, InstallOperation&& install)
{
    using Status = ReviewedPackageInstallStatus;
    ScopedPackageIdentityLock lock(path, paths.staging, identity);
    const auto needsConfirmation = [&]() {
        return !lock.MatchesPathIdentity() && !allowUnlockedReview;
    };
    const auto actualHash = widget::detail::HashPackageFile(path, true);
    if (actualHash.empty()) return {Status::Unreadable, {}, {}};
    if (actualHash != sha256) return {Status::Changed, {}, {}};
    widget::WidgetPackageManager validator(paths);
    widget::PackageManifest current;
    const auto report = validator.ValidateArchive(path, &current);
    if (!report.Ok()) return {Status::Invalid, report.ToJson(), {}};
    if (current.id != manifest.id || current.version != manifest.version)
        return {Status::Changed, {}, {}};
    const auto verifiedHash = widget::detail::HashPackageFile(path, true);
    if (verifiedHash.empty()) return {Status::Unreadable, {}, {}};
    if (verifiedHash != sha256) return {Status::Changed, {}, {}};
    if (needsConfirmation())
        return {Status::NeedsLockConfirmation, {}, lock.Details()};
    return {std::forward<InstallOperation>(install)() ? Status::Installed : Status::InstallFailed, {}, {}};
}
} // namespace snowdesktop::winui::widgets_page_backend_detail
