// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>

#include "workshop_project.h"
#include "bridge_json.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace snowdesktop::steam_bridge
{
namespace
{
std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr,
        nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr,
        nullptr) != length)
        return {};
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) != length)
        return {};
    return result;
}

std::filesystem::path DefaultRoot()
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
            KF_FLAG_DEFAULT, nullptr, &value)))
        return {};
    const std::filesystem::path result =
        std::filesystem::path(value) / L"SnowDesktop" /
        L"SteamWorkshopManager";
    CoTaskMemFree(value);
    return result;
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ContainsReparsePoint(const std::filesystem::path& absolutePath)
{
    std::filesystem::path current = absolutePath.root_path();
    for (const auto& component : absolutePath.relative_path())
    {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return true;
    }
    return false;
}

bool IsBundledComponentDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    const auto root = path.parent_path();
    return std::filesystem::is_regular_file(
        root / L"snowdesktop-lua-widget" / L"SKILL.md", error) && !error;
}

std::optional<std::filesystem::path> SafeDirectory(
    const std::filesystem::path& input, std::string& error)
{
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(input, ec);
    if (ec || absolute.empty())
    {
        error = "cannot resolve the project directory";
        return std::nullopt;
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (ec || !std::filesystem::is_directory(canonical, ec) || ec)
    {
        error = "project directory does not exist";
        return std::nullopt;
    }
    if (IsBundledComponentDirectory(canonical))
    {
        error = "bundled components are not creator projects";
        return std::nullopt;
    }
    if (ContainsReparsePoint(absolute) || ContainsReparsePoint(canonical))
    {
        error = "project directory cannot be a symbolic link or junction";
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(canonical / L"widget.json", ec) || ec)
    {
        error = "project directory does not contain widget.json";
        return std::nullopt;
    }
    return canonical;
}

std::string NewUuid()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) return {};
    wchar_t text[40]{};
    if (StringFromGUID2(value, text, static_cast<int>(std::size(text))) <= 0)
        return {};
    std::wstring uuid(text);
    if (!uuid.empty() && uuid.front() == L'{') uuid.erase(uuid.begin());
    if (!uuid.empty() && uuid.back() == L'}') uuid.pop_back();
    return WideToUtf8(uuid);
}

JsonValue ProjectToJson(const WorkshopProject& project)
{
    JsonValue value = JsonValue::Object();
    value.object["localId"] = JsonValue::String(project.localId);
    value.object["sourceDirectory"] = JsonValue::String(
        WideToUtf8(project.sourceDirectory.wstring()));
    value.object["primaryPreview"] = JsonValue::String(
        WideToUtf8(project.primaryPreview.wstring()));
    JsonValue tags = JsonValue::Array();
    for (const auto& tag : project.tags)
        tags.array.push_back(JsonValue::String(tag));
    value.object["tags"] = std::move(tags);
    value.object["publishedFileId"] = project.publishedFileId
        ? JsonValue::String(std::to_string(*project.publishedFileId))
        : JsonValue{};
    value.object["packageId"] = JsonValue::String(project.packageId);
    value.object["lastPublishedVersion"] =
        JsonValue::String(project.lastPublishedVersion);
    value.object["lastPublishedSha256"] =
        JsonValue::String(project.lastPublishedSha256);
    value.object["lastPublishedAt"] =
        JsonValue::String(project.lastPublishedAt);
    return value;
}

bool ReadRequiredString(const JsonValue& object, std::string_view key,
    std::string& output, std::string& error)
{
    const auto value = JsonString(object, key);
    if (!value)
    {
        error = "project store field is missing or invalid: " +
            std::string(key);
        return false;
    }
    output = *value;
    return true;
}

bool ProjectFromJson(const JsonValue& value, WorkshopProject& project,
    std::string& error)
{
    if (!value.IsObject())
    {
        error = "project store contains a non-object project";
        return false;
    }
    std::string source;
    std::string preview;
    if (!ReadRequiredString(value, "localId", project.localId, error) ||
        !ReadRequiredString(value, "sourceDirectory", source, error) ||
        !ReadRequiredString(value, "primaryPreview", preview, error) ||
        !ReadRequiredString(value, "packageId", project.packageId, error) ||
        !ReadRequiredString(value, "lastPublishedVersion",
            project.lastPublishedVersion, error) ||
        !ReadRequiredString(value, "lastPublishedSha256",
            project.lastPublishedSha256, error) ||
        !ReadRequiredString(value, "lastPublishedAt",
            project.lastPublishedAt, error))
        return false;
    project.sourceDirectory = Utf8ToWide(source);
    project.primaryPreview = Utf8ToWide(preview);
    if ((!source.empty() && project.sourceDirectory.empty()) ||
        (!preview.empty() && project.primaryPreview.empty()))
    {
        error = "project store contains invalid UTF-8 paths";
        return false;
    }
    const JsonValue* tags = value.Find("tags");
    if (!tags || !tags->IsArray())
    {
        error = "project store tags field is invalid";
        return false;
    }
    for (const auto& tag : tags->array)
    {
        if (!tag.IsString())
        {
            error = "project store contains an invalid tag";
            return false;
        }
        project.tags.push_back(tag.string);
    }
    const JsonValue* itemId = value.Find("publishedFileId");
    if (!itemId)
    {
        error = "project store publishedFileId field is missing";
        return false;
    }
    if (itemId->IsString() && !itemId->string.empty())
    {
        try
        {
            std::size_t consumed = 0;
            const auto parsed = std::stoull(itemId->string, &consumed);
            if (parsed == 0 || consumed != itemId->string.size())
                throw std::invalid_argument("id");
            project.publishedFileId = parsed;
        }
        catch (...)
        {
            error = "project store contains an invalid PublishedFileId";
            return false;
        }
    }
    else if (!itemId->IsNull())
    {
        error = "project store publishedFileId must be a string or null";
        return false;
    }
    return !project.localId.empty();
}
}

ProjectStore::ProjectStore(std::filesystem::path root)
    : root_(root.empty() ? DefaultRoot() : std::move(root))
{
}

std::filesystem::path ProjectStore::StorePath() const
{
    return root_ / L"projects.json";
}

bool ProjectStore::Load(std::string& error)
{
    projects_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(StorePath(), ec)) return !ec;
    if (IsReparsePoint(StorePath()))
    {
        error = "projects.json cannot be a symbolic link or reparse point";
        return false;
    }
    std::ifstream input(StorePath(), std::ios::binary);
    if (!input)
    {
        error = "cannot open projects.json";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    if (text.size() > 4u * 1024u * 1024u)
    {
        error = "projects.json exceeds the 4 MiB safety limit";
        return false;
    }
    JsonValue root;
    if (!ParseJson(text, root, error) || !root.IsObject())
    {
        if (error.empty()) error = "projects.json root must be an object";
        return false;
    }
    const auto schema = JsonUnsigned(root, "schemaVersion");
    const JsonValue* projects = root.Find("projects");
    if (!schema || *schema != kProjectStoreSchemaVersion ||
        !projects || !projects->IsArray())
    {
        error = "unsupported or malformed project store schema";
        return false;
    }
    for (const auto& value : projects->array)
    {
        WorkshopProject project;
        if (!ProjectFromJson(value, project, error))
        {
            projects_.clear();
            return false;
        }
        if (IsBundledComponentDirectory(project.sourceDirectory))
            continue;
        if (std::any_of(projects_.begin(), projects_.end(),
            [&](const WorkshopProject& current)
            { return current.localId == project.localId; }))
        {
            error = "project store contains duplicate local IDs";
            projects_.clear();
            return false;
        }
        projects_.push_back(std::move(project));
    }
    return true;
}

bool ProjectStore::Save(std::string& error) const
{
    if (root_.empty())
    {
        error = "LocalAppData is unavailable";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec || IsReparsePoint(root_))
    {
        error = "project store directory is unavailable or is a reparse point";
        return false;
    }
    JsonValue root = JsonValue::Object();
    root.object["schemaVersion"] =
        JsonValue::Number(kProjectStoreSchemaVersion);
    JsonValue projects = JsonValue::Array();
    for (const auto& project : projects_)
        projects.array.push_back(ProjectToJson(project));
    root.object["projects"] = std::move(projects);
    const std::string serialized = WriteJson(root, 2) + "\n";
    const auto target = StorePath();
    const auto temporary = root_ / L"projects.json.tmp";
    const auto backup = root_ / L"projects.json.bak";
    if (IsReparsePoint(target) || IsReparsePoint(temporary) ||
        IsReparsePoint(backup))
    {
        error = "project store files cannot be symbolic links or reparse points";
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(serialized.data(),
                static_cast<std::streamsize>(serialized.size())) ||
            !output.flush())
        {
            error = "cannot write projects.json.tmp";
            return false;
        }
    }
    if (std::filesystem::exists(target, ec) &&
        !CopyFileW(target.c_str(), backup.c_str(), FALSE))
    {
        std::filesystem::remove(temporary, ec);
        error = "cannot create projects.json.bak";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(temporary, ec);
        error = "cannot atomically replace projects.json";
        return false;
    }
    return true;
}

bool ProjectStore::AddDirectory(const std::filesystem::path& source,
    WorkshopProject*& project, std::string& error)
{
    project = nullptr;
    const auto safe = SafeDirectory(source, error);
    if (!safe) return false;
    const auto found = std::find_if(projects_.begin(), projects_.end(),
        [&](const WorkshopProject& current)
        { return current.sourceDirectory == *safe; });
    if (found != projects_.end())
    {
        project = &*found;
        return true;
    }
    WorkshopProject added;
    added.localId = NewUuid();
    added.sourceDirectory = *safe;
    if (added.localId.empty())
    {
        error = "cannot generate a local project UUID";
        return false;
    }
    projects_.push_back(std::move(added));
    project = &projects_.back();
    return true;
}

bool ProjectStore::Discover(const std::filesystem::path& developmentRoot,
    std::size_t& added, std::string& error)
{
    added = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(developmentRoot, ec) || ec)
        return true;
    if (IsReparsePoint(developmentRoot))
    {
        error = "development root cannot be a symbolic link or junction";
        return false;
    }
    if (std::filesystem::is_regular_file(developmentRoot /
            L"snowdesktop-lua-widget" / L"SKILL.md", ec) && !ec)
        return true;
    for (std::filesystem::directory_iterator iterator(developmentRoot,
             std::filesystem::directory_options::skip_permission_denied, ec),
         end; iterator != end; iterator.increment(ec))
    {
        if (ec) break;
        if (!iterator->is_directory(ec) || ec ||
            IsReparsePoint(iterator->path()))
            continue;
        if (!std::filesystem::is_regular_file(
                iterator->path() / L"widget.json", ec) || ec)
            continue;
        const auto before = projects_.size();
        WorkshopProject* project = nullptr;
        std::string addError;
        if (AddDirectory(iterator->path(), project, addError) &&
            projects_.size() != before)
            ++added;
    }
    if (ec)
    {
        error = "cannot enumerate the development root";
        return false;
    }
    return true;
}

bool ProjectStore::Remove(std::string_view localId, std::string& error)
{
    const auto found = std::find_if(projects_.begin(), projects_.end(),
        [&](const WorkshopProject& project)
        { return project.localId == localId; });
    if (found == projects_.end())
    {
        error = "local project record was not found";
        return false;
    }
    projects_.erase(found);
    return true;
}

std::string BuildWorkshopMetadata(
    std::string_view packageId, std::string_view version)
{
    JsonValue root = JsonValue::Object();
    root.object["format"] = JsonValue::String("snowdesktop-widget");
    root.object["artifact"] = JsonValue::String("package.snowwidget");
    root.object["protocolVersion"] = JsonValue::Number(1);
    root.object["packageId"] = JsonValue::String(std::string(packageId));
    root.object["version"] = JsonValue::String(std::string(version));
    return WriteJson(root, -1);
}

std::optional<WorkshopMetadata> ParseWorkshopMetadata(
    std::string_view metadata, std::string& error)
{
    JsonValue root;
    if (!ParseJson(metadata, root, error) || !root.IsObject())
    {
        if (error.empty()) error = "Workshop metadata must be a JSON object";
        return std::nullopt;
    }
    const auto format = JsonString(root, "format");
    const auto artifact = JsonString(root, "artifact");
    const auto packageId = JsonString(root, "packageId");
    const auto version = JsonString(root, "version");
    if (!format || *format != "snowdesktop-widget" ||
        !artifact || *artifact != "package.snowwidget" ||
        !packageId || packageId->empty() || !version || version->empty())
    {
        error = "Workshop metadata is not a complete SnowDesktop component association";
        return std::nullopt;
    }
    return WorkshopMetadata{ *packageId, *version };
}

bool CanBindWorkshopItem(const WorkshopProject& project,
    std::string_view metadata, std::uint64_t ownerSteamId,
    std::uint64_t currentSteamId, std::uint32_t consumerAppId,
    std::uint32_t currentAppId, std::string& error)
{
    if (ownerSteamId == 0 || ownerSteamId != currentSteamId)
    {
        error = "Workshop item is not owned by the current Steam user";
        return false;
    }
    if (consumerAppId == 0 || consumerAppId != currentAppId)
    {
        error = "Workshop item belongs to a different Consumer App ID";
        return false;
    }
    const auto parsed = ParseWorkshopMetadata(metadata, error);
    if (!parsed) return false;
    if (!project.packageId.empty() && parsed->packageId != project.packageId)
    {
        error = "Workshop metadata packageId does not match the local component UUID";
        return false;
    }
    return true;
}
}
