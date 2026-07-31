#include "layout_storage.h"

#include "atomic_file.h"
#include "json_value.h"

#include <array>

namespace snowdesktop::layout_storage
{
namespace
{
bool ValidateObjectArray(const JsonValue& root, std::string_view name,
    std::string* error)
{
    const JsonValue* value = root.Find(name);
    if (!value) return true;
    if (!value->IsArray())
    {
        if (error) *error = std::string(name) + " must be an array";
        return false;
    }
    for (const JsonValue& entry : value->array)
    {
        if (!entry.IsObject())
        {
            if (error)
                *error = std::string(name) +
                    " must contain only objects";
            return false;
        }
    }
    return true;
}

bool ValidateStringArray(const JsonValue& root, std::string_view name,
    std::string* error)
{
    const JsonValue* value = root.Find(name);
    if (!value) return true;
    if (!value->IsArray())
    {
        if (error) *error = std::string(name) + " must be an array";
        return false;
    }
    for (const JsonValue& entry : value->array)
    {
        if (!entry.IsString())
        {
            if (error)
                *error = std::string(name) +
                    " must contain only strings";
            return false;
        }
    }
    return true;
}

bool ReadAndValidate(const std::filesystem::path& path,
    std::string& contents, std::string& error)
{
    if (!atomic_file::ReadAll(path, contents, &error))
        return false;
    return ValidateDocument(contents, &error);
}
}

std::filesystem::path BackupPath(const std::filesystem::path& layoutPath)
{
    std::filesystem::path backup = layoutPath;
    backup += L".last-good";
    return backup;
}

bool ValidateDocument(std::string_view contents, std::string* error)
{
    if (error) error->clear();
    JsonValue root;
    if (!ParseJson(contents, root, error))
        return false;
    if (!root.IsObject())
    {
        if (error) *error = "layout root must be an object";
        return false;
    }
    if (const JsonValue* schema = root.Find("layoutSchemaVersion"))
    {
        if (!schema->IsNumber() || schema->number != 1.0)
        {
            if (error)
                *error = "unsupported layoutSchemaVersion";
            return false;
        }
    }
    for (const std::string_view name :
        { "pages", "items", "widgets", "dockEntries" })
    {
        if (!ValidateObjectArray(root, name, error))
            return false;
    }
    return ValidateStringArray(root, "navTabOrder", error);
}

LoadResult LoadDocument(const std::filesystem::path& layoutPath,
    std::string& contents)
{
    contents.clear();
    std::error_code existsError;
    const bool primaryExists =
        std::filesystem::exists(layoutPath, existsError);
    std::string primaryError;
    if (primaryExists &&
        ReadAndValidate(layoutPath, contents, primaryError))
    {
        return { LoadStatus::LoadedPrimary, {} };
    }

    const auto backupPath = BackupPath(layoutPath);
    const bool backupExists =
        std::filesystem::exists(backupPath, existsError);
    std::string backupError;
    if (backupExists &&
        ReadAndValidate(backupPath, contents, backupError))
    {
        return {
            LoadStatus::RecoveredBackup,
            primaryExists ? primaryError : "primary layout is missing"
        };
    }

    contents.clear();
    if (!primaryExists && !backupExists)
        return { LoadStatus::Missing, {} };
    std::string error = "layout is invalid";
    if (primaryExists && !primaryError.empty())
        error += ": primary: " + primaryError;
    if (backupExists && !backupError.empty())
        error += "; backup: " + backupError;
    return { LoadStatus::Invalid, std::move(error) };
}

bool SaveDocument(const std::filesystem::path& layoutPath,
    std::string_view contents, std::string* error)
{
    if (!ValidateDocument(contents, error))
        return false;

    std::string previous;
    std::string previousError;
    const bool hasValidPrevious =
        atomic_file::ReadAll(layoutPath, previous, &previousError) &&
        ValidateDocument(previous, nullptr);
    return atomic_file::WriteAll(layoutPath, contents,
        hasValidPrevious ? BackupPath(layoutPath) :
            std::filesystem::path{},
        error);
}
}
