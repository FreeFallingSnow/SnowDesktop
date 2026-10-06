#pragma once

#include <windows.h>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace snowdesktop::widget::detail
{
// A catalogue snapshot is reusable only while all local directory watches are
// healthy and its manager revision still matches. Runtime package validation
// deliberately does not use this cache.
class CatalogRefresh
{
public:
    using Revision = std::shared_ptr<const char>;

    bool CanReuse(const Revision& revision) const
    {
        return valid_ && revision && revision == revision_ && !Changed();
    }

    void Begin(const std::array<std::filesystem::path, 3>& paths)
    {
        valid_ = false;
        for (size_t i = 0; i < roots_.size(); ++i)
        {
            auto& root = roots_[i];
            if (root.path != paths[i] || root.stamp != ReadStamp(paths[i]) ||
                (!paths[i].empty() && !root.Healthy()))
                root.Reset(paths[i]);
            else
            {
                root.Rearm(root.tree);
                root.Rearm(root.parent);
            }
        }
    }

    void Complete(Revision revision)
    {
        revision_ = std::move(revision);
        valid_ = !Changed();
    }

private:
    struct Stamp
    {
        DWORD attributes, createdLow, createdHigh, modifiedLow, modifiedHigh;
        bool operator==(const Stamp&) const = default;
    };

    static std::optional<Stamp> ReadStamp(const std::filesystem::path& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (path.empty() || !GetFileAttributesExW(path.c_str(),
                GetFileExInfoStandard, &data))
            return {};
        return Stamp{data.dwFileAttributes, data.ftCreationTime.dwLowDateTime,
            data.ftCreationTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime,
            data.ftLastWriteTime.dwHighDateTime};
    }

    struct Root
    {
        std::filesystem::path path;
        std::optional<Stamp> stamp;
        HANDLE tree = INVALID_HANDLE_VALUE;
        HANDLE parent = INVALID_HANDLE_VALUE;

        ~Root() { Close(); }
        Root() = default;
        Root(const Root&) = delete;
        Root& operator=(const Root&) = delete;

        bool Healthy() const
        {
            return tree != INVALID_HANDLE_VALUE && parent != INVALID_HANDLE_VALUE;
        }

        void Close()
        {
            if (tree != INVALID_HANDLE_VALUE) FindCloseChangeNotification(tree);
            if (parent != INVALID_HANDLE_VALUE) FindCloseChangeNotification(parent);
            tree = parent = INVALID_HANDLE_VALUE;
        }

        static void Rearm(HANDLE& handle)
        {
            if (handle == INVALID_HANDLE_VALUE) return;
            const DWORD state = WaitForSingleObject(handle, 0);
            if ((state == WAIT_OBJECT_0 && !FindNextChangeNotification(handle)) ||
                (state != WAIT_OBJECT_0 && state != WAIT_TIMEOUT))
            {
                FindCloseChangeNotification(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }

        void Reset(const std::filesystem::path& source)
        {
            Close();
            path = source;
            stamp = ReadStamp(path);
            if (!stamp || (stamp->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (stamp->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return;
            // Remote filesystems can omit notifications. Retain full refresh
            // there, and whenever notifications cannot be created or queried.
            std::wstring volume(32768, L'\0');
            if (!GetVolumePathNameW(path.c_str(), volume.data(),
                    static_cast<DWORD>(volume.size())) ||
                GetDriveTypeW(volume.c_str()) == DRIVE_REMOTE)
                return;
            constexpr DWORD changes = FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_SECURITY;
            tree = FindFirstChangeNotificationW(path.c_str(), TRUE, changes);
            // A tree watch does not report replacement of its own root.
            parent = FindFirstChangeNotificationW(path.parent_path().c_str(),
                FALSE, FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        }
    };

    bool Changed() const
    {
        for (const auto& root : roots_)
        {
            if (root.path.empty()) continue;
            if (!root.Healthy() || ReadStamp(root.path) != root.stamp ||
                WaitForSingleObject(root.tree, 0) != WAIT_TIMEOUT ||
                WaitForSingleObject(root.parent, 0) != WAIT_TIMEOUT)
                return true;
        }
        return false;
    }

    std::array<Root, 3> roots_;
    Revision revision_;
    bool valid_ = false;
};
} // namespace snowdesktop::widget::detail
