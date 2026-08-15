#include "widget_filesystem_handle_store.h"

#include "atomic_file.h"
#include "json_value.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::string_view kHandlePrefix = "filesystem:";

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

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), result.data(),
            size) != size)
        return {};
    return result;
}

std::string EscapeJson(std::string_view value)
{
    std::ostringstream output;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<unsigned>(character)
                    << std::dec;
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

std::optional<WidgetFilesystemHandleKind> ParseKind(
    std::string_view value)
{
    if (value == "file") return WidgetFilesystemHandleKind::File;
    if (value == "folder") return WidgetFilesystemHandleKind::Folder;
    return std::nullopt;
}

std::optional<WidgetFilesystemHandleAccess> ParseAccess(
    std::string_view value)
{
    if (value == "read") return WidgetFilesystemHandleAccess::Read;
    if (value == "write") return WidgetFilesystemHandleAccess::Write;
    if (value == "readWrite")
        return WidgetFilesystemHandleAccess::ReadWrite;
    return std::nullopt;
}

const JsonValue* StringField(const JsonValue& object, const char* name)
{
    const JsonValue* value = object.Find(name);
    return value && value->IsString() ? value : nullptr;
}
}

WidgetFilesystemHandleStore::WidgetFilesystemHandleStore(
    std::filesystem::path registryPath)
    : registryPath_(std::move(registryPath))
{
}

bool WidgetFilesystemHandleStore::Load(std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    entries_.clear();
    if (registryPath_.empty()) return true;

    std::error_code filesystemError;
    if (!std::filesystem::exists(registryPath_, filesystemError))
        return !filesystemError;
    if (filesystemError ||
        !std::filesystem::is_regular_file(registryPath_, filesystemError))
    {
        error = "filesystem handle registry is not a regular file";
        return false;
    }

    std::string text;
    if (!atomic_file::ReadAll(registryPath_, text, &error)) return false;
    JsonValue root;
    if (!ParseJson(text, root, &error) || !root.IsObject())
    {
        if (error.empty()) error = "filesystem handle registry is invalid";
        return false;
    }
    const JsonValue* schema = root.Find("schemaVersion");
    const JsonValue* entries = root.Find("entries");
    if (!schema || !schema->IsNumber() || !std::isfinite(schema->number) ||
        schema->number != 1.0 || !entries || !entries->IsArray() ||
        entries->array.size() > MaximumEntries)
    {
        error = "filesystem handle registry schema is invalid";
        return false;
    }

    std::unordered_map<std::string, std::size_t> perInstance;
    for (const JsonValue& value : entries->array)
    {
        if (!value.IsObject())
        {
            error = "filesystem handle registry entry is invalid";
            entries_.clear();
            return false;
        }
        const JsonValue* handle = StringField(value, "handle");
        const JsonValue* instanceId = StringField(value, "instanceId");
        const JsonValue* packageId = StringField(value, "packageId");
        const JsonValue* path = StringField(value, "path");
        const JsonValue* kind = StringField(value, "kind");
        const JsonValue* access = StringField(value, "access");
        const auto parsedKind = kind ? ParseKind(kind->string) : std::nullopt;
        const auto parsedAccess = access
            ? ParseAccess(access->string) : std::nullopt;
        const std::wstring widePath = path
            ? Utf8ToWide(path->string) : std::wstring{};
        if (!handle || !instanceId || !packageId || !path ||
            !parsedKind || !parsedAccess || instanceId->string.empty() ||
            packageId->string.empty() || widePath.empty() ||
            !IsOpaqueHandle(handle->string) ||
            !std::filesystem::path(widePath).is_absolute() ||
            ++perInstance[instanceId->string] > MaximumEntriesPerInstance)
        {
            error = "filesystem handle registry entry is invalid";
            entries_.clear();
            return false;
        }
        WidgetFilesystemHandleEntry entry;
        entry.handle = handle->string;
        entry.owner = { instanceId->string, packageId->string };
        entry.path = std::filesystem::path(widePath).lexically_normal();
        entry.kind = *parsedKind;
        entry.access = *parsedAccess;
        if (!entries_.emplace(entry.handle, std::move(entry)).second)
        {
            error = "filesystem handle registry contains duplicate handles";
            entries_.clear();
            return false;
        }
    }
    return true;
}

WidgetFilesystemHandleGrantResult WidgetFilesystemHandleStore::Grant(
    WidgetFilesystemHandleOwner owner, const std::filesystem::path& path,
    WidgetFilesystemHandleKind kind, WidgetFilesystemHandleAccess access)
{
    if (owner.instanceId.empty() || owner.packageId.empty())
        return { std::nullopt, "invalidOwner" };
    const auto normalized = NormalizeGrantedPath(path, kind);
    if (!normalized) return { std::nullopt, "invalidSelection" };

    std::scoped_lock lock(mutex_);
    std::size_t ownerCount = 0;
    for (const auto& [_, entry] : entries_)
    {
        if (entry.owner.instanceId != owner.instanceId) continue;
        ++ownerCount;
        if (entry.owner.packageId == owner.packageId &&
            entry.path == *normalized && entry.kind == kind &&
            entry.access == access)
            return { entry, {} };
    }
    if (entries_.size() >= MaximumEntries ||
        ownerCount >= MaximumEntriesPerInstance)
        return { std::nullopt, "handleQuotaExceeded" };

    WidgetFilesystemHandleEntry entry;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        entry.handle = GenerateHandle();
        if (!entry.handle.empty() && !entries_.contains(entry.handle)) break;
        entry.handle.clear();
    }
    if (entry.handle.empty())
        return { std::nullopt, "handleGenerationFailed" };
    entry.owner = std::move(owner);
    entry.path = *normalized;
    entry.kind = kind;
    entry.access = access;
    entries_.emplace(entry.handle, entry);
    std::string error;
    if (!SaveLocked(error))
    {
        entries_.erase(entry.handle);
        return { std::nullopt,
            error.empty() ? "handlePersistenceFailed" : std::move(error) };
    }
    return { std::move(entry), {} };
}

std::optional<WidgetFilesystemHandleEntry>
WidgetFilesystemHandleStore::Resolve(
    const WidgetFilesystemHandleOwner& owner,
    std::string_view handle) const
{
    if (owner.instanceId.empty() || owner.packageId.empty() ||
        !IsOpaqueHandle(handle))
        return std::nullopt;
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(std::string(handle));
    if (found == entries_.end() ||
        found->second.owner.instanceId != owner.instanceId ||
        found->second.owner.packageId != owner.packageId)
        return std::nullopt;
    return found->second;
}

bool WidgetFilesystemHandleStore::Revoke(
    const WidgetFilesystemHandleOwner& owner,
    std::string_view handle, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    const auto found = entries_.find(std::string(handle));
    if (found == entries_.end() ||
        found->second.owner.instanceId != owner.instanceId ||
        found->second.owner.packageId != owner.packageId)
        return false;
    const auto backup = found->second;
    entries_.erase(found);
    if (SaveLocked(error)) return true;
    entries_.emplace(backup.handle, backup);
    return false;
}

std::size_t WidgetFilesystemHandleStore::RevokeInstance(
    std::string_view instanceId, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    if (instanceId.empty()) return 0;
    const auto backup = entries_;
    const std::size_t removed = std::erase_if(entries_,
        [instanceId](const auto& item) {
            return item.second.owner.instanceId == instanceId;
        });
    if (removed == 0 || SaveLocked(error)) return removed;
    entries_ = backup;
    return 0;
}

std::size_t WidgetFilesystemHandleStore::RevokePackage(
    std::string_view packageId, std::string& error)
{
    std::scoped_lock lock(mutex_);
    error.clear();
    if (packageId.empty()) return 0;
    const auto backup = entries_;
    const std::size_t removed = std::erase_if(entries_,
        [packageId](const auto& item) {
            return item.second.owner.packageId == packageId;
        });
    if (removed == 0 || SaveLocked(error)) return removed;
    entries_ = backup;
    return 0;
}

std::size_t WidgetFilesystemHandleStore::Size() const noexcept
{
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

bool WidgetFilesystemHandleStore::IsOpaqueHandle(
    std::string_view handle) noexcept
{
    if (!handle.starts_with(kHandlePrefix) ||
        handle.size() != kHandlePrefix.size() + 32)
        return false;
    return std::all_of(handle.begin() +
            static_cast<std::ptrdiff_t>(kHandlePrefix.size()), handle.end(),
        [](const unsigned char character) {
            return std::isdigit(character) ||
                (character >= 'a' && character <= 'f');
        });
}

std::string_view WidgetFilesystemHandleStore::KindName(
    WidgetFilesystemHandleKind kind) noexcept
{
    return kind == WidgetFilesystemHandleKind::Folder ? "folder" : "file";
}

std::string_view WidgetFilesystemHandleStore::AccessName(
    WidgetFilesystemHandleAccess access) noexcept
{
    switch (access)
    {
    case WidgetFilesystemHandleAccess::Write: return "write";
    case WidgetFilesystemHandleAccess::ReadWrite: return "readWrite";
    default: return "read";
    }
}

bool WidgetFilesystemHandleStore::SaveLocked(std::string& error) const
{
    error.clear();
    if (registryPath_.empty()) return true;
    std::vector<const WidgetFilesystemHandleEntry*> sorted;
    sorted.reserve(entries_.size());
    for (const auto& [_, entry] : entries_) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left,
        const auto* right) { return left->handle < right->handle; });

    std::ostringstream output;
    output << "{\n  \"schemaVersion\": 1,\n  \"entries\": [";
    for (std::size_t index = 0; index < sorted.size(); ++index)
    {
        const auto& entry = *sorted[index];
        const std::string path = WideToUtf8(entry.path.wstring());
        if (path.empty())
        {
            error = "handlePersistenceFailed";
            return false;
        }
        output << (index == 0 ? "\n" : ",\n")
            << "    {\"handle\":\"" << EscapeJson(entry.handle)
            << "\",\"instanceId\":\""
            << EscapeJson(entry.owner.instanceId)
            << "\",\"packageId\":\""
            << EscapeJson(entry.owner.packageId)
            << "\",\"path\":\"" << EscapeJson(path)
            << "\",\"kind\":\"" << KindName(entry.kind)
            << "\",\"access\":\"" << AccessName(entry.access)
            << "\"}";
    }
    if (!sorted.empty()) output << '\n';
    output << "  ]\n}\n";
    if (!atomic_file::WriteAll(registryPath_, output.str(), {}, &error))
    {
        if (error.empty()) error = "handlePersistenceFailed";
        return false;
    }
    return true;
}

std::optional<std::filesystem::path>
WidgetFilesystemHandleStore::NormalizeGrantedPath(
    const std::filesystem::path& path, WidgetFilesystemHandleKind kind)
{
    if (path.empty()) return std::nullopt;
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error || absolute.empty()) return std::nullopt;
    absolute = absolute.lexically_normal();

    if (kind == WidgetFilesystemHandleKind::Folder)
    {
        if (!std::filesystem::is_directory(absolute, error) || error)
            return std::nullopt;
        const auto canonical = std::filesystem::weakly_canonical(
            absolute, error);
        return error || canonical.empty()
            ? std::nullopt
            : std::optional<std::filesystem::path>(canonical);
    }

    if (std::filesystem::exists(absolute, error))
    {
        if (error || !std::filesystem::is_regular_file(absolute, error) ||
            error)
            return std::nullopt;
        const auto canonical = std::filesystem::weakly_canonical(
            absolute, error);
        return error || canonical.empty()
            ? std::nullopt
            : std::optional<std::filesystem::path>(canonical);
    }
    if (error) return std::nullopt;

    const auto parent = std::filesystem::weakly_canonical(
        absolute.parent_path(), error);
    if (error || parent.empty() ||
        !std::filesystem::is_directory(parent, error) || error ||
        absolute.filename().empty())
        return std::nullopt;
    return parent / absolute.filename();
}

std::string WidgetFilesystemHandleStore::GenerateHandle()
{
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(),
            static_cast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result(kHandlePrefix);
    result.reserve(kHandlePrefix.size() + random.size() * 2);
    for (const unsigned char byte : random)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0F]);
    }
    return result;
}
}
