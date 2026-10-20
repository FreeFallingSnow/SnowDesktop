#include "widget_filesystem_watch_origin.h"

#include <optional>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace
{
struct PathLease
{
    std::vector<HANDLE> handles;
    BY_HANDLE_FILE_INFORMATION leaf{};
    ~PathLease() { for (const auto handle : handles) CloseHandle(handle); }
    bool Open(const std::filesystem::path& absolute, std::optional<bool> directory,
        std::string& error)
    {
        auto current = absolute.root_path();
        std::vector<std::filesystem::path> prefixes{current};
        for (const auto& part : absolute.relative_path())
        {
            current /= part;
            prefixes.push_back(current);
        }
        for (std::size_t index = 0; index < prefixes.size(); ++index)
        {
            const auto handle = CreateFileW(prefixes[index].c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                error = "watchPathUnavailable";
                return false;
            }
            handles.push_back(handle);
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle, &information))
            {
                error = "watchPathUnavailable";
                return false;
            }
            if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                error = "reparsePointDenied";
                return false;
            }
            const bool isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((index + 1 < prefixes.size() && !isDirectory) ||
                (index + 1 == prefixes.size() && directory && *directory != isDirectory))
            {
                error = "invalidDirectory";
                return false;
            }
            leaf = information;
        }
        return !handles.empty();
    }
};

bool SameIdentity(const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right)
{
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
        left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
}
}

struct WidgetFilesystemWatchOrigin::Impl
{
    std::filesystem::path directory;
    HANDLE handle = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

WidgetFilesystemWatchOrigin::WidgetFilesystemWatchOrigin(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
WidgetFilesystemWatchOrigin::~WidgetFilesystemWatchOrigin() = default;
HANDLE WidgetFilesystemWatchOrigin::TakeDirectoryHandle() const noexcept
{
    return std::exchange(impl_->handle, INVALID_HANDLE_VALUE);
}

std::shared_ptr<const WidgetFilesystemWatchOrigin> WidgetFilesystemWatchOrigin::Open(
    const std::filesystem::path& directory, std::string& error)
{
    error.clear();
    std::error_code ec;
    auto absolute = std::filesystem::absolute(directory, ec).lexically_normal();
    if (ec || !absolute.is_absolute())
    {
        error = "invalidDirectory";
        return {};
    }
    PathLease lease;
    if (!lease.Open(absolute, true, error)) return {};
    auto impl = std::make_unique<Impl>();
    impl->directory = std::move(absolute);
    impl->handle = CreateFileW(impl->directory.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl->handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(impl->handle, &impl->identity) ||
        !SameIdentity(lease.leaf, impl->identity))
    {
        error = "watchOpenFailed";
        return {};
    }
    return std::shared_ptr<const WidgetFilesystemWatchOrigin>(new WidgetFilesystemWatchOrigin(std::move(impl)));
}

WidgetFilesystemHandleGrantResult WidgetFilesystemWatchOrigin::GrantChild(
    WidgetFilesystemHandleStore& store, WidgetFilesystemHandleOwner owner,
    std::wstring_view name, WidgetFilesystemHandleAccess access) const
{
    if (name.empty() || name == L"." || name == L".." ||
        name.find_first_of(L"\\/:") != std::wstring_view::npos ||
        name.find(L'\0') != std::wstring_view::npos)
        return {{}, false, "invalidPath"};
    PathLease directoryLease;
    std::string error;
    if (!directoryLease.Open(impl_->directory, true, error)) return {{}, false, std::move(error)};
    if (!SameIdentity(directoryLease.leaf, impl_->identity)) return {{}, false, "watchOriginChanged"};
    const auto child = impl_->directory / std::filesystem::path(name);
    PathLease childLease;
    if (!childLease.Open(child, std::nullopt, error)) return {{}, false, std::move(error)};
    const auto kind = (childLease.leaf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        ? WidgetFilesystemHandleKind::Folder : WidgetFilesystemHandleKind::File;
    return store.Grant(std::move(owner), child, kind, access);
}
}
