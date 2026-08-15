#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
enum class WidgetFilesystemHandleKind
{
    File,
    Folder,
};

enum class WidgetFilesystemHandleAccess
{
    Read,
    Write,
    ReadWrite,
};

struct WidgetFilesystemHandleOwner
{
    std::string instanceId;
    std::string packageId;
};

struct WidgetFilesystemHandleEntry
{
    std::string handle;
    WidgetFilesystemHandleOwner owner;
    std::filesystem::path path;
    WidgetFilesystemHandleKind kind = WidgetFilesystemHandleKind::File;
    WidgetFilesystemHandleAccess access =
        WidgetFilesystemHandleAccess::Read;
};

struct WidgetFilesystemHandleGrantResult
{
    std::optional<WidgetFilesystemHandleEntry> entry;
    bool created = false;
    std::string error;

    explicit operator bool() const noexcept
    {
        return entry.has_value() && error.empty();
    }
};

class WidgetFilesystemHandleStore
{
public:
    static constexpr std::size_t MaximumEntries = 4096;
    static constexpr std::size_t MaximumEntriesPerInstance = 256;

    explicit WidgetFilesystemHandleStore(
        std::filesystem::path registryPath = {});

    bool Load(std::string& error);
    WidgetFilesystemHandleGrantResult Grant(
        WidgetFilesystemHandleOwner owner,
        const std::filesystem::path& path,
        WidgetFilesystemHandleKind kind,
        WidgetFilesystemHandleAccess access);
    std::optional<WidgetFilesystemHandleEntry> Resolve(
        const WidgetFilesystemHandleOwner& owner,
        std::string_view handle) const;
    bool Revoke(const WidgetFilesystemHandleOwner& owner,
        std::string_view handle, std::string& error);
    std::size_t RevokeInstance(
        std::string_view instanceId, std::string& error);
    std::size_t RevokePackage(
        std::string_view packageId, std::string& error);
    std::size_t Size() const noexcept;

    static bool IsOpaqueHandle(std::string_view handle) noexcept;
    static std::string_view KindName(
        WidgetFilesystemHandleKind kind) noexcept;
    static std::string_view AccessName(
        WidgetFilesystemHandleAccess access) noexcept;

private:
    bool SaveLocked(std::string& error) const;
    static std::optional<std::filesystem::path> NormalizeGrantedPath(
        const std::filesystem::path& path,
        WidgetFilesystemHandleKind kind);
    static std::string GenerateHandle();

    std::filesystem::path registryPath_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, WidgetFilesystemHandleEntry> entries_;
};
}
