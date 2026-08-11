#include "layout_storage.h"

#include "atomic_file.h"
#include "json_value.h"

#include <cmath>
#include <limits>

namespace snowdesktop::layout_storage
{
namespace
{
bool Fail(std::string* error, std::string_view path,
    std::string_view expectation)
{
    if (error)
    {
        *error = std::string(path);
        *error += " ";
        *error += expectation;
    }
    return false;
}

bool DecodeInteger(const JsonValue& value, int& output)
{
    if (!value.IsNumber() || !std::isfinite(value.number) ||
        std::trunc(value.number) != value.number ||
        value.number < static_cast<double>(std::numeric_limits<int>::min()) ||
        value.number > static_cast<double>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    output = static_cast<int>(value.number);
    return true;
}

bool DecodeFloat(const JsonValue& value, float& output)
{
    if (!value.IsNumber() || !std::isfinite(value.number) ||
        value.number < -static_cast<double>(
            std::numeric_limits<float>::max()) ||
        value.number > static_cast<double>(
            std::numeric_limits<float>::max()))
    {
        return false;
    }
    output = static_cast<float>(value.number);
    return true;
}

bool ReadRequiredString(const JsonValue& object, std::string_view name,
    std::string_view path, std::string& output, std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value)
        return Fail(error, path, "is required");
    if (!value->IsString())
        return Fail(error, path, "must be a string");
    output = value->string;
    return true;
}

bool ReadString(const JsonValue& object, std::string_view name,
    std::string_view path, std::string& output, std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!value->IsString())
        return Fail(error, path, "must be a string");
    output = value->string;
    return true;
}

bool ReadOptionalString(const JsonValue& object, std::string_view name,
    std::string_view path, std::optional<std::string>& output,
    std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!value->IsString())
        return Fail(error, path, "must be a string");
    output = value->string;
    return true;
}

bool ReadRequiredInteger(const JsonValue& object, std::string_view name,
    std::string_view path, int& output, std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value)
        return Fail(error, path, "is required");
    if (!DecodeInteger(*value, output))
        return Fail(error, path, "must be an integer");
    return true;
}

bool ReadInteger(const JsonValue& object, std::string_view name,
    std::string_view path, int& output, std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!DecodeInteger(*value, output))
        return Fail(error, path, "must be an integer");
    return true;
}

bool ReadOptionalInteger(const JsonValue& object, std::string_view name,
    std::string_view path, std::optional<int>& output,
    std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    int decoded = 0;
    if (!DecodeInteger(*value, decoded))
        return Fail(error, path, "must be an integer");
    output = decoded;
    return true;
}

bool ReadOptionalFloat(const JsonValue& object, std::string_view name,
    std::string_view path, std::optional<float>& output,
    std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    float decoded = 0.0f;
    if (!DecodeFloat(*value, decoded))
        return Fail(error, path, "must be a finite number");
    output = decoded;
    return true;
}

bool ReadBoolean(const JsonValue& object, std::string_view name,
    std::string_view path, bool& output, std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!value->IsBoolean())
        return Fail(error, path, "must be a boolean");
    output = value->boolean;
    return true;
}

bool ReadOptionalBoolean(const JsonValue& object, std::string_view name,
    std::string_view path, std::optional<bool>& output,
    std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!value->IsBoolean())
        return Fail(error, path, "must be a boolean");
    output = value->boolean;
    return true;
}

bool ReadOptionalRootBoolean(const JsonValue& object,
    std::string_view name, std::optional<bool>& output,
    std::string* error)
{
    return ReadOptionalBoolean(
        object, name, name, output, error);
}

bool ReadStringArray(const JsonValue& object, std::string_view name,
    std::string_view path, std::vector<std::string>& output,
    std::string* error)
{
    const JsonValue* value = object.Find(name);
    if (!value) return true;
    if (!value->IsArray())
        return Fail(error, path, "must be an array");
    output.clear();
    output.reserve(value->array.size());
    for (size_t index = 0; index < value->array.size(); ++index)
    {
        if (!value->array[index].IsString())
        {
            return Fail(error,
                std::string(path) + "[" + std::to_string(index) + "]",
                "must be a string");
        }
        output.push_back(value->array[index].string);
    }
    return true;
}

bool ReadObjectArray(const JsonValue& root, std::string_view name,
    const JsonValue*& output, std::string* error)
{
    output = root.Find(name);
    if (!output) return true;
    if (!output->IsArray())
        return Fail(error, name, "must be an array");
    for (size_t index = 0; index < output->array.size(); ++index)
    {
        if (!output->array[index].IsObject())
        {
            return Fail(error,
                std::string(name) + "[" + std::to_string(index) + "]",
                "must be an object");
        }
    }
    return true;
}

bool DecodePages(const JsonValue& root, Document& document,
    std::string* error)
{
    const JsonValue* pages = nullptr;
    if (!ReadObjectArray(root, "pages", pages, error)) return false;
    if (!pages) return true;
    document.pages.reserve(pages->array.size());
    for (size_t index = 0; index < pages->array.size(); ++index)
    {
        const JsonValue& object = pages->array[index];
        const std::string path = "pages[" + std::to_string(index) + "]";
        PageRecord record;
        if (!ReadRequiredString(object, "id", path + ".id",
                record.id, error) ||
            !ReadOptionalInteger(object, "columns", path + ".columns",
                record.columns, error) ||
            !ReadOptionalInteger(object, "rows", path + ".rows",
                record.rows, error))
        {
            return false;
        }
        document.pages.push_back(std::move(record));
    }
    return true;
}

bool DecodeItems(const JsonValue& root, Document& document,
    std::string* error)
{
    const JsonValue* items = nullptr;
    if (!ReadObjectArray(root, "items", items, error)) return false;
    if (!items) return true;
    document.items.reserve(items->array.size());
    for (size_t index = 0; index < items->array.size(); ++index)
    {
        const JsonValue& object = items->array[index];
        const std::string path = "items[" + std::to_string(index) + "]";
        ItemRecord record;
        if (!ReadRequiredString(object, "key", path + ".key",
                record.key, error) ||
            !ReadOptionalString(object, "page", path + ".page",
                record.page, error) ||
            !ReadOptionalInteger(object, "x", path + ".x",
                record.column, error) ||
            !ReadOptionalInteger(object, "y", path + ".y",
                record.row, error) ||
            !ReadInteger(object, "w", path + ".w",
                record.width, error) ||
            !ReadInteger(object, "h", path + ".h",
                record.height, error))
        {
            return false;
        }
        const int gridParts = static_cast<int>(record.page.has_value()) +
            static_cast<int>(record.column.has_value()) +
            static_cast<int>(record.row.has_value());
        if (gridParts != 0 && gridParts != 3)
            return Fail(error, path,
                "must provide page, x, and y together");
        document.items.push_back(std::move(record));
    }
    return true;
}

bool DecodeWidgets(const JsonValue& root, Document& document,
    std::string* error)
{
    const JsonValue* widgets = nullptr;
    if (!ReadObjectArray(root, "widgets", widgets, error)) return false;
    if (!widgets) return true;
    document.widgets.reserve(widgets->array.size());
    for (size_t index = 0; index < widgets->array.size(); ++index)
    {
        const JsonValue& object = widgets->array[index];
        const std::string path = "widgets[" + std::to_string(index) + "]";
        WidgetRecord record;
        if (!ReadRequiredString(object, "id", path + ".id",
                record.id, error) ||
            !ReadRequiredString(object, "page", path + ".page",
                record.page, error) ||
            !ReadRequiredInteger(object, "x", path + ".x",
                record.column, error) ||
            !ReadRequiredInteger(object, "y", path + ".y",
                record.row, error) ||
            !ReadInteger(object, "w", path + ".w",
                record.width, error) ||
            !ReadInteger(object, "h", path + ".h",
                record.height, error) ||
            !ReadString(object, "type", path + ".type",
                record.type, error) ||
            !ReadOptionalString(object, "title", path + ".title",
                record.title, error) ||
            !ReadOptionalString(object, "customTitle",
                path + ".customTitle", record.customTitle, error) ||
            !ReadOptionalString(object, "titleMode", path + ".titleMode",
                record.titleMode, error) ||
            !ReadString(object, "sourceFolderPath",
                path + ".sourceFolderPath", record.sourceFolderPath,
                error) ||
            !ReadString(object, "packageId", path + ".packageId",
                record.packageId, error) ||
            !ReadString(object, "packageSourceProvider",
                path + ".packageSourceProvider",
                record.packageSourceProvider, error) ||
            !ReadString(object, "packageSourceExternalItemId",
                path + ".packageSourceExternalItemId",
                record.packageSourceExternalItemId, error) ||
            !ReadString(object, "packageSourceUrl",
                path + ".packageSourceUrl",
                record.packageSourceUrl, error) ||
            !ReadString(object, "scriptPath", path + ".scriptPath",
                record.scriptPath, error) ||
            !ReadString(object, "legacyScriptPath",
                path + ".legacyScriptPath", record.legacyScriptPath,
                error) ||
            !ReadString(object, "activeCategory",
                path + ".activeCategory", record.activeCategory,
                error) ||
            !ReadInteger(object, "scrollOffset", path + ".scrollOffset",
                record.scrollOffset, error) ||
            !ReadInteger(object, "tabScrollOffset",
                path + ".tabScrollOffset", record.tabScrollOffset,
                error) ||
            !ReadInteger(object, "folderSortMode",
                path + ".folderSortMode", record.folderSortMode,
                error) ||
            !ReadBoolean(object, "folderSortAscending",
                path + ".folderSortAscending", record.folderSortAscending,
                error) ||
            !ReadBoolean(object, "autoCollect", path + ".autoCollect",
                record.autoCollect, error) ||
            !ReadBoolean(object, "listMode", path + ".listMode",
                record.listMode, error) ||
            !ReadBoolean(object, "dateHeaders", path + ".dateHeaders",
                record.dateHeaders, error) ||
            !ReadBoolean(object, "showFileCategories",
                path + ".showFileCategories", record.showFileCategories,
                error) ||
            !ReadBoolean(object, "showSearchBox", path + ".showSearchBox",
                record.showSearchBox, error) ||
            !ReadBoolean(object, "showOnHoverOnly",
                path + ".showOnHoverOnly", record.showOnHoverOnly,
                error) ||
            !ReadBoolean(object, "privacyMode", path + ".privacyMode",
                record.privacyMode, error) ||
            !ReadBoolean(object, "scrollContainerMode",
                path + ".scrollContainerMode", record.scrollContainerMode,
                error) ||
            !ReadBoolean(object, "keepWhenDesktopHidden",
                path + ".keepWhenDesktopHidden",
                record.keepWhenDesktopHidden, error) ||
            !ReadOptionalBoolean(object, "showTitle", path + ".showTitle",
                record.showTitle, error) ||
            !ReadOptionalBoolean(object, "bottomBarHover",
                path + ".bottomBarHover", record.bottomBarHover, error) ||
            !ReadOptionalBoolean(object, "userRenamed",
                path + ".userRenamed", record.userRenamed, error) ||
            !ReadStringArray(object, "items", path + ".items",
                record.items, error) ||
            !ReadStringArray(object, "childWidgets",
                path + ".childWidgets", record.childWidgets, error))
        {
            return false;
        }
        document.widgets.push_back(std::move(record));
    }
    return true;
}

bool DecodeDockEntries(const JsonValue& root, Document& document,
    std::string* error)
{
    const JsonValue* entries = nullptr;
    if (!ReadObjectArray(root, "dockEntries", entries, error)) return false;
    if (!entries) return true;
    document.dockEntries.reserve(entries->array.size());
    for (size_t index = 0; index < entries->array.size(); ++index)
    {
        const JsonValue& object = entries->array[index];
        const std::string path =
            "dockEntries[" + std::to_string(index) + "]";
        DockRecord record;
        if (!ReadRequiredString(object, "type", path + ".type",
                record.type, error) ||
            !ReadRequiredString(object, "ref", path + ".ref",
                record.reference, error) ||
            !ReadBoolean(object, "keepOnDesktop",
                path + ".keepOnDesktop", record.keepOnDesktop, error) ||
            !ReadInteger(object, "folderSortMode",
                path + ".folderSortMode", record.folderSortMode, error) ||
            !ReadBoolean(object, "folderSortAscending",
                path + ".folderSortAscending",
                record.folderSortAscending, error) ||
            !ReadStringArray(object, "folderItems",
                path + ".folderItems", record.folderItems, error))
        {
            return false;
        }
        document.dockEntries.push_back(std::move(record));
    }
    return true;
}

bool DecodeDocument(const JsonValue& root, Document& document,
    std::string* error)
{
    if (!root.IsObject())
        return Fail(error, "layout root", "must be an object");

    Document decoded;
    std::optional<int> sourceSchemaVersion;
    if (!ReadOptionalInteger(root, "layoutSchemaVersion",
            "layoutSchemaVersion", sourceSchemaVersion, error))
    {
        return false;
    }
    decoded.sourceSchemaVersion = sourceSchemaVersion.value_or(0);
    if (decoded.sourceSchemaVersion != 0 &&
        decoded.sourceSchemaVersion != kCurrentSchemaVersion)
    {
        return Fail(error, "layoutSchemaVersion", "is unsupported");
    }

    if (!ReadOptionalInteger(root, "widgetTitleSchemaVersion",
            "widgetTitleSchemaVersion",
            decoded.widgetTitleSchemaVersion, error) ||
        !ReadOptionalInteger(root, "widgetContentOptionsSchemaVersion",
            "widgetContentOptionsSchemaVersion",
            decoded.widgetContentOptionsSchemaVersion, error) ||
        !ReadOptionalString(root, "firstPageMonitor", "firstPageMonitor",
            decoded.firstPageMonitor, error) ||
        !ReadOptionalString(root, "lastPageMonitor", "lastPageMonitor",
            decoded.lastPageMonitor, error) ||
        !ReadOptionalRootBoolean(root, "dockEnabled",
            decoded.dockEnabled, error) ||
        !ReadOptionalFloat(root, "itemFontSize", "itemFontSize",
            decoded.itemFontSize, error) ||
        !ReadOptionalFloat(root, "itemFontWeight", "itemFontWeight",
            decoded.itemFontWeight, error) ||
        !ReadOptionalFloat(root, "iconSpacing", "iconSpacing",
            decoded.iconSpacing, error) ||
        !ReadOptionalFloat(root, "componentSpacing", "componentSpacing",
            decoded.componentSpacing, error) ||
        !ReadOptionalInteger(root, "shortcutArrowMode", "shortcutArrowMode",
            decoded.shortcutArrowMode, error) ||
        !ReadOptionalRootBoolean(root, "iconBeautifyEnabled",
            decoded.iconBeautifyEnabled, error) ||
        !ReadOptionalInteger(root, "iconBeautifyPreset", "iconBeautifyPreset",
            decoded.iconBeautifyPreset, error) ||
        !ReadOptionalInteger(root, "iconBeautifyMode", "iconBeautifyMode",
            decoded.iconBeautifyMode, error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgOpacity",
            "iconBeautifyBgOpacity", decoded.iconBeautifyBgOpacity,
            error) ||
        !ReadOptionalRootBoolean(root, "iconBeautifyGradientEnabled",
            decoded.iconBeautifyGradientEnabled, error) ||
        !ReadOptionalInteger(root, "iconBeautifyGradientDirection",
            "iconBeautifyGradientDirection",
            decoded.iconBeautifyGradientDirection, error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgStartR",
            "iconBeautifyBgStartR", decoded.iconBeautifyBgStartR,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgStartG",
            "iconBeautifyBgStartG", decoded.iconBeautifyBgStartG,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgStartB",
            "iconBeautifyBgStartB", decoded.iconBeautifyBgStartB,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgEndR",
            "iconBeautifyBgEndR", decoded.iconBeautifyBgEndR,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgEndG",
            "iconBeautifyBgEndG", decoded.iconBeautifyBgEndG,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyBgEndB",
            "iconBeautifyBgEndB", decoded.iconBeautifyBgEndB,
            error) ||
        !ReadOptionalInteger(root, "iconBeautifyShape",
            "iconBeautifyShape", decoded.iconBeautifyShape, error) ||
        !ReadOptionalFloat(root, "iconBeautifyContentScale",
            "iconBeautifyContentScale", decoded.iconBeautifyContentScale,
            error) ||
        !ReadOptionalInteger(root, "iconBeautifyFinish",
            "iconBeautifyFinish", decoded.iconBeautifyFinish, error) ||
        !ReadOptionalRootBoolean(root, "iconBeautifyOutlineEnabled",
            decoded.iconBeautifyOutlineEnabled, error) ||
        !ReadOptionalInteger(root, "iconBeautifyOutlineMode",
            "iconBeautifyOutlineMode", decoded.iconBeautifyOutlineMode,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyOutlineWidth",
            "iconBeautifyOutlineWidth", decoded.iconBeautifyOutlineWidth,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyOutlineOpacity",
            "iconBeautifyOutlineOpacity", decoded.iconBeautifyOutlineOpacity,
            error) ||
        !ReadOptionalFloat(root, "iconBeautifyOutlineR",
            "iconBeautifyOutlineR", decoded.iconBeautifyOutlineR, error) ||
        !ReadOptionalFloat(root, "iconBeautifyOutlineG",
            "iconBeautifyOutlineG", decoded.iconBeautifyOutlineG, error) ||
        !ReadOptionalFloat(root, "iconBeautifyOutlineB",
            "iconBeautifyOutlineB", decoded.iconBeautifyOutlineB, error) ||
        !ReadOptionalFloat(root, "iconBeautifyShadowStrength",
            "iconBeautifyShadowStrength", decoded.iconBeautifyShadowStrength,
            error) ||
        !DecodePages(root, decoded, error) ||
        !DecodeItems(root, decoded, error) ||
        !DecodeWidgets(root, decoded, error) ||
        !DecodeDockEntries(root, decoded, error) ||
        !ReadStringArray(root, "navTabOrder", "navTabOrder",
            decoded.navTabOrder, error))
    {
        return false;
    }

    // Schema 0 used the same structural fields but had no explicit version.
    // Decoding it into the current in-memory model is its v0 -> v1 migration.
    decoded.schemaVersion = kCurrentSchemaVersion;
    document = std::move(decoded);
    return true;
}

bool ReadAndDecode(const std::filesystem::path& path,
    Document& document, std::string& error)
{
    std::string contents;
    if (!atomic_file::ReadAll(path, contents, &error))
        return false;
    return ParseDocument(contents, document, &error);
}
}

std::filesystem::path BackupPath(const std::filesystem::path& layoutPath)
{
    std::filesystem::path backup = layoutPath;
    backup += L".last-good";
    return backup;
}

bool ParseDocument(std::string_view contents, Document& document,
    std::string* error)
{
    if (error) error->clear();
    JsonValue root;
    if (!ParseJson(contents, root, error))
        return false;
    return DecodeDocument(root, document, error);
}

bool ValidateDocument(std::string_view contents, std::string* error)
{
    Document document;
    return ParseDocument(contents, document, error);
}

LoadResult LoadDocument(const std::filesystem::path& layoutPath,
    Document& document)
{
    document = {};
    std::error_code existsError;
    const bool primaryExists =
        std::filesystem::exists(layoutPath, existsError);
    std::string primaryError;
    if (primaryExists &&
        ReadAndDecode(layoutPath, document, primaryError))
    {
        return { LoadStatus::LoadedPrimary, {} };
    }

    const auto backupPath = BackupPath(layoutPath);
    const bool backupExists =
        std::filesystem::exists(backupPath, existsError);
    std::string backupError;
    if (backupExists &&
        ReadAndDecode(backupPath, document, backupError))
    {
        return {
            LoadStatus::RecoveredBackup,
            primaryExists ? primaryError : "primary layout is missing"
        };
    }

    document = {};
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
    Document replacement;
    if (!ParseDocument(contents, replacement, error))
        return false;

    std::string previous;
    std::string previousError;
    Document previousDocument;
    const bool hasValidPrevious =
        atomic_file::ReadAll(layoutPath, previous, &previousError) &&
        ParseDocument(previous, previousDocument, nullptr);
    return atomic_file::WriteAll(layoutPath, contents,
        hasValidPrevious ? BackupPath(layoutPath) :
            std::filesystem::path{},
        error);
}
}
